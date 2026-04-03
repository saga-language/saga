// Copyright 2026 Rob Thornton
// SPDX-License-Identifier: MIT
//
// Shared utilities for subcommand implementations.
// Only include from src/cmd/*.cpp (needs SAGA_* compile-time defines).

#pragma once

#include <algorithm>
#include <filesystem>
#include <format>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Install-prefix detection
// In dev builds SAGA_STD_SGI_DIR / SAGA_STD_LIB are baked in at compile time.
// In an installed build we derive them from the running binary:
//   <prefix>/bin/saga  →  prefix is two levels up.
// ---------------------------------------------------------------------------

inline fs::path detect_install_prefix(const char *argv0) {
  std::error_code ec;
  fs::path binary = fs::weakly_canonical(argv0, ec);
  if (ec)
    binary = fs::absolute(argv0);
  return binary.parent_path().parent_path();
}

inline std::string saga_std_sgi_dir(const char *argv0) {
#ifdef SAGA_STD_SGI_DIR
  (void)argv0;
  return SAGA_STD_SGI_DIR;
#else
  return (detect_install_prefix(argv0) / "lib" / "saga" / "std").string();
#endif
}

inline std::string saga_std_lib(const char *argv0) {
#ifdef SAGA_STD_LIB
  (void)argv0;
  return SAGA_STD_LIB;
#else
  if (!argv0 || !*argv0)
    return {};
  return (detect_install_prefix(argv0) / "lib" / "saga" / "libsaga_std.a")
      .string();
#endif
}

inline std::string saga_runtime_lib() {
#ifdef SAGA_RUNTIME_LIB
  return SAGA_RUNTIME_LIB;
#else
  return {};
#endif
}

// ---------------------------------------------------------------------------
// Platform helpers
// ---------------------------------------------------------------------------

inline bool is_platform_file(const std::string &stem) {
  for (auto &p : {"_linux", "_darwin", "_windows"})
    if (stem.ends_with(p))
      return true;
  return false;
}

inline std::string current_platform() {
#if defined(__linux__)
  return "_linux";
#elif defined(__APPLE__)
  return "_darwin";
#elif defined(_WIN32)
  return "_windows";
#else
  return "";
#endif
}

// ---------------------------------------------------------------------------
// Collect all platform-filtered .sg files from a directory (sorted).
// ---------------------------------------------------------------------------

inline std::vector<fs::path> collect_sg_files(const fs::path &dir) {
  std::vector<fs::path> result;
  for (auto &entry : fs::directory_iterator(dir)) {
    std::string stem = entry.path().stem().string();
    if (entry.is_regular_file() && entry.path().extension() == ".sg" &&
        (!is_platform_file(stem) || stem.ends_with(current_platform())))
      result.push_back(entry.path());
  }
  std::sort(result.begin(), result.end());
  return result;
}

// ---------------------------------------------------------------------------
// Load source files from a file-or-directory input path into a FileSet.
// Returns the resolved package_dir, or empty string on error.
// Prints its own error messages.
// ---------------------------------------------------------------------------

#include "frontend/file.hpp"
#include "frontend/fileset.hpp"

inline std::string load_sources(const std::string &source_path,
                                 mc::FileSet &fileset) {
  fs::path input(source_path);
  std::string package_dir;

  if (fs::is_directory(input)) {
    package_dir = fs::canonical(input).string();
    auto sg_files = collect_sg_files(input);
    if (sg_files.empty()) {
      std::cerr << std::format("Error: no .sg files in '{}'\n", source_path);
      return {};
    }
    for (auto &sg : sg_files) {
      auto file = mc::File::from_path(sg.string());
      if (!file) {
        std::cerr << std::format("Error: cannot open '{}'\n", sg.string());
        return {};
      }
      fileset.add_file(std::move(file));
    }
  } else {
    auto file = mc::File::from_path(source_path);
    if (!file) {
      std::cerr << std::format("Error: cannot open '{}'\n", source_path);
      return {};
    }
    std::error_code ec;
    package_dir = fs::canonical(input, ec).parent_path().string();
    fileset.add_file(std::move(file));
  }
  return package_dir;
}

// ---------------------------------------------------------------------------
// Wire up an Analyzer's search paths (source + sgi), including auto-detected
// stdlib SGI dir.
// ---------------------------------------------------------------------------

#include "semantic/analyzer.hpp"

inline void setup_analyzer_paths(mc::Analyzer &analyzer,
                                  const std::string &package_dir,
                                  const std::vector<std::string> &search_paths,
                                  const std::vector<std::string> &sgi_paths,
                                  const char *prog) {
  if (!package_dir.empty()) {
    analyzer.current_package_dir = package_dir;
    fs::path parent = fs::path(package_dir).parent_path();
    if (!parent.empty())
      analyzer.package_resolver->search_paths.push_back(parent.string());
  }
  for (auto &sp : search_paths)
    analyzer.package_resolver->search_paths.push_back(sp);
  // Always include std SGI dir.
  {
    std::string std_sgi = saga_std_sgi_dir(prog);
    if (fs::is_directory(std_sgi))
      analyzer.package_resolver->sgi_search_paths.push_back(std_sgi);
  }
  for (auto &sp : sgi_paths)
    analyzer.package_resolver->sgi_search_paths.push_back(sp);
}

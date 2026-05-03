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
#include "pkg/manifest.hpp"

inline std::string load_sources(const std::string &source_path,
                                 saga::FileSet &fileset) {
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
      auto file = saga::File::from_path(sg.string());
      if (!file) {
        std::cerr << std::format("Error: cannot open '{}'\n", sg.string());
        return {};
      }
      fileset.add_file(std::move(file));
    }
  } else {
    auto file = saga::File::from_path(source_path);
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

// ---------------------------------------------------------------------------
// Apply manifest dependencies to search/sgi path vectors.
// Call before setup_analyzer_paths so all deps are visible to the analyzer.
// ---------------------------------------------------------------------------

// Compile a single dep package to .o + .sgi using the running compiler binary.
// out_dir is created if absent. Returns true on success.
inline bool compile_dep_package(const char *prog,
                                 const std::string &src_dir,
                                 const std::string &pkg_name,
                                 const std::string &out_dir,
                                 const std::vector<std::string> &extra_sgi_dirs) {
  std::error_code ec;
  fs::create_directories(out_dir, ec);

  // Resolve the binary to an absolute path so it works from any CWD.
  std::string binary = fs::weakly_canonical(prog, ec).string();
  if (ec) binary = prog;

  std::string cmd =
      std::format("\"{}\" --lib -o \"{}/{}.o\"", binary, out_dir, pkg_name);
  for (auto &d : extra_sgi_dirs)
    cmd += std::format(" --sgi-path \"{}\"", d);
  cmd += std::format(" \"{}\"", src_dir);

  return std::system(cmd.c_str()) == 0;
}

inline void apply_manifest_deps(const char *prog,
                                 const std::string &start_dir,
                                 std::vector<std::string> &search_paths,
                                 std::vector<std::string> &sgi_search_paths) {
  auto manifest_path = saga::find_manifest(start_dir);
  if (!manifest_path) return;

  auto manifest = saga::Manifest::load(*manifest_path);
  if (!manifest) return;

  fs::path project_dir = fs::path(*manifest_path).parent_path();
  // Local deps are compiled into <project>/.build/<name>/
  fs::path build_cache = project_dir / ".build";

  for (auto &dep : manifest->dependencies) {
    if (dep.is_local()) {
      fs::path dep_src = fs::weakly_canonical(project_dir / dep.path);
      fs::path dep_out = build_cache / dep.name;

      std::string sgi_file = (dep_out / (dep.name + ".sgi")).string();
      std::string obj_file = (dep_out / (dep.name + ".o")).string();

      // Compile if artifacts are absent (incremental via BuildGraph is
      // available for multi-package projects; here we just compile once).
      if (!fs::is_regular_file(sgi_file) || !fs::is_regular_file(obj_file)) {
        compile_dep_package(prog, dep_src.string(), dep.name,
                            dep_out.string(), sgi_search_paths);
      }

      if (fs::is_directory(dep_out) &&
          std::find(sgi_search_paths.begin(), sgi_search_paths.end(),
                    dep_out.string()) == sgi_search_paths.end())
        sgi_search_paths.push_back(dep_out.string());

    } else if (dep.is_remote() && !dep.commit.empty()) {
      fs::path cache = saga::pkg_cache_dir(dep);
      if (fs::is_directory(cache) &&
          std::find(sgi_search_paths.begin(), sgi_search_paths.end(),
                    cache.string()) == sgi_search_paths.end())
        sgi_search_paths.push_back(cache.string());
    }
  }
}

inline void setup_analyzer_paths(saga::Analyzer &analyzer,
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

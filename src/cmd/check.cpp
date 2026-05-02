// Copyright 2026 Rob Thornton
// SPDX-License-Identifier: MIT
//
// saga check — parse and type-check only; no codegen.
// Exit code 0 = no errors, 1 = errors found.

#include "cmd/cmd.hpp"
#include "cmd/common.hpp"

#include "frontend/fileset.hpp"
#include "frontend/parser.hpp"
#include "semantic/analyzer.hpp"

#include <filesystem>
#include <format>
#include <iostream>
#include <string>

namespace fs = std::filesystem;

static void usage_check() {
  std::cerr << "Usage: saga check [options] <source.sg | directory>\n"
               "\n"
               "Options:\n"
               "  -I <dir>          Add directory to package search path\n"
               "  --sgi-path <dir>  Add directory to .sgi search path\n"
               "  -v                Print 'no errors' on success\n";
}

int cmd_check(const char *prog, int argc, char **argv) {
  std::string source_path;
  bool verbose = false;
  std::vector<std::string> search_paths;
  std::vector<std::string> sgi_search_paths;

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "-v")               { verbose = true; }
    else if (arg == "--sgi-path" && i + 1 < argc)
      sgi_search_paths.push_back(argv[++i]);
    else if (arg == "-I" && i + 1 < argc)
      search_paths.push_back(argv[++i]);
    else if (arg[0] == '-') {
      std::cerr << std::format("Unknown option: {}\n", arg);
      usage_check();
      return 1;
    } else {
      source_path = arg;
    }
  }

  if (source_path.empty()) {
    usage_check();
    return 1;
  }

  // Apply deps from the nearest project.saga.
  apply_manifest_deps(prog, fs::current_path().string(), search_paths,
                      sgi_search_paths);

  saga::FileSet fileset;
  std::string package_dir = load_sources(source_path, fileset);
  if (package_dir.empty() && fileset.files.empty())
    return 1;

  saga::Parser parser(fileset);
  auto ast = parser.parse();
  if (!ast) {
    std::cerr << "Error: parse failed\n";
    return 1;
  }
  if (!parser.errors.errors.empty()) {
    parser.errors.print_errors();
    return 1;
  }

  saga::Analyzer analyzer(fileset);
  setup_analyzer_paths(analyzer, package_dir, search_paths, sgi_search_paths,
                       prog);
  analyzer.analyze(*ast);

  if (!analyzer.errors.errors.empty()) {
    analyzer.errors.print_errors();
    return 1;
  }

  if (verbose)
    std::cerr << std::format("No errors in {}\n", source_path);
  return 0;
}

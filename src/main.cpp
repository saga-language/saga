// Copyright 2026 Rob Thornton
// SPDX-License-Identifier: MIT

#include "frontend/file.hpp"
#include "frontend/fileset.hpp"
#include "frontend/parser.hpp"
#include "ir/codegen.hpp"
#include "semantic/analyzer.hpp"

#include <filesystem>
#include <format>
#include <iostream>
#include <string>

namespace fs = std::filesystem;

static void usage(const char *prog) {
  std::cerr << std::format("Usage: {} [options] <source.sg | directory>\n", prog);
  std::cerr << "Options:\n";
  std::cerr << "  --emit-ir    Write LLVM IR to <name>.ll instead of compiling\n";
  std::cerr << "  --dump-ir    Print LLVM IR to stderr\n";
  std::cerr << "  -o <file>    Output file name (default: a.out)\n";
  std::cerr << "  -I <dir>     Add directory to package search path\n";
}

int main(int argc, char **argv) {
  // ── Parse CLI arguments ────────────────────────────────────────────

  std::string source_path;
  std::string output_path = "a.out";
  bool emit_ir = false;
  bool dump_ir = false;
  std::vector<std::string> search_paths;

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--emit-ir") {
      emit_ir = true;
    } else if (arg == "--dump-ir") {
      dump_ir = true;
    } else if (arg == "-o" && i + 1 < argc) {
      output_path = argv[++i];
    } else if (arg == "-I" && i + 1 < argc) {
      search_paths.push_back(argv[++i]);
    } else if (arg[0] == '-') {
      std::cerr << std::format("Unknown option: {}\n", arg);
      usage(argv[0]);
      return 1;
    } else {
      source_path = arg;
    }
  }

  if (source_path.empty()) {
    usage(argv[0]);
    return 1;
  }

  // ── Frontend: parse ────────────────────────────────────────────────

  mc::FileSet fileset;

  // Determine if the input is a directory (package) or a single file.
  fs::path input_path(source_path);
  std::string package_dir;

  if (fs::is_directory(input_path)) {
    // Multi-file package: load all .sg files in the directory.
    package_dir = fs::canonical(input_path).string();
    std::vector<fs::path> sg_files;
    for (auto &entry : fs::directory_iterator(input_path)) {
      if (entry.is_regular_file() && entry.path().extension() == ".sg") {
        sg_files.push_back(entry.path());
      }
    }
    std::sort(sg_files.begin(), sg_files.end());
    if (sg_files.empty()) {
      std::cerr << std::format("Error: no .sg files in '{}'\n", source_path);
      return 1;
    }
    for (auto &sg : sg_files) {
      auto file = mc::File::from_path(sg.string());
      if (!file) {
        std::cerr << std::format("Error: cannot open '{}'\n", sg.string());
        return 1;
      }
      fileset.add_file(std::move(file));
    }
  } else {
    // Single file.
    package_dir = fs::canonical(input_path).parent_path().string();
    auto file = mc::File::from_path(source_path);
    if (!file) {
      std::cerr << std::format("Error: cannot open '{}'\n", source_path);
      return 1;
    }
    fileset.add_file(std::move(file));
  }

  mc::Parser parser(fileset);
  auto ast = parser.parse();
  if (!ast) {
    std::cerr << "Error: parse failed\n";
    return 1;
  }

  if (!parser.errors.errors.empty()) {
    parser.errors.print_errors();
    return 1;
  }

  // ── Semantic analysis ──────────────────────────────────────────────

  mc::Analyzer analyzer(fileset);

  // Set up package resolver with search paths.
  // Always include the parent of the current package for sibling imports.
  if (!package_dir.empty()) {
    analyzer.current_package_dir = package_dir;
    fs::path parent = fs::path(package_dir).parent_path();
    if (!parent.empty()) {
      analyzer.package_resolver->search_paths.push_back(parent.string());
    }
  }
  for (auto &sp : search_paths) {
    analyzer.package_resolver->search_paths.push_back(sp);
  }

  analyzer.analyze(*ast);

  if (!analyzer.errors.errors.empty()) {
    analyzer.errors.print_errors();
    return 1;
  }

  // ── Code generation ────────────────────────────────────────────────

  std::string module_name;
  if (fs::is_directory(input_path)) {
    module_name = fs::path(source_path).filename().string();
  } else {
    module_name = fs::path(source_path).stem().string();
  }
  mc::CodeGen codegen(module_name, analyzer);
  codegen.emit(*ast);

  if (dump_ir) {
    codegen.dump();
  }

  if (emit_ir) {
    std::string ir_path = module_name + ".ll";
    if (!codegen.write_ir(ir_path)) {
      std::cerr << std::format("Error: cannot write IR to '{}'\n", ir_path);
      return 1;
    }
    std::cerr << std::format("Wrote {}\n", ir_path);
    return 0;
  }

  // ── Compile to object file, then link ──────────────────────────────

  std::string obj_path = module_name + ".o";
  if (!codegen.write_object(obj_path)) {
    std::cerr << "Error: failed to emit object file\n";
    return 1;
  }

  // Link with the system linker (cc), including the Saga runtime.
#ifndef SAGA_RUNTIME_LIB
#define SAGA_RUNTIME_LIB ""
#endif
  std::string runtime_lib = SAGA_RUNTIME_LIB;
  std::string link_cmd =
      std::format("cc {} {} -o {} -no-pie", obj_path, runtime_lib, output_path);
  int link_status = std::system(link_cmd.c_str());

  // Clean up the temporary object file.
  std::filesystem::remove(obj_path);

  if (link_status != 0) {
    std::cerr << "Error: linking failed\n";
    return 1;
  }

  return 0;
}

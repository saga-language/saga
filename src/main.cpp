// Copyright 2026 Rob Thornton
// SPDX-License-Identifier: MIT

#include "frontend/file.hpp"
#include "frontend/fileset.hpp"
#include "frontend/parser.hpp"
#include "ir/codegen.hpp"
#include "semantic/analyzer.hpp"
#include "semantic/sgi.hpp"

#include <filesystem>
#include <format>
#include <iostream>
#include <string>

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Platform helpers (mirrors logic in analyzer.cpp)
// ---------------------------------------------------------------------------

static bool is_platform_file(const std::string &stem) {
  static const std::vector<std::string> platforms = {"_linux", "_darwin",
                                                     "_windows"};
  for (auto &p : platforms) {
    if (stem.ends_with(p))
      return true;
  }
  return false;
}

static std::string current_platform() {
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

static void usage(const char *prog) {
  std::cerr << std::format("Usage: {} [options] <source.sg | directory>\n",
                           prog);
  std::cerr << "Options:\n";
  std::cerr
      << "  --emit-ir    Write LLVM IR to <name>.ll instead of compiling\n";
  std::cerr << "  --emit-obj    Write package to object file\n";
  std::cerr << "  --lib        Build as library (.o + .sgi, no link)\n";
  std::cerr << "  --dump-ir    Print LLVM IR to stderr\n";
  std::cerr << "  -o <file>    Output file name (default: a.out)\n";
  std::cerr << "  -I <dir>     Add directory to package search path\n";
  std::cerr
      << "  --sgi-path <dir>  Add directory to .sgi search path\n";
}

int main(int argc, char **argv) {
  // ── Parse CLI arguments ────────────────────────────────────────────

  std::string source_path;
  std::string output_path;
  bool emit_ir = false;
  bool dump_ir = false;
  bool emit_obj = false;
  bool lib_mode = false;
  std::vector<std::string> search_paths;
  std::vector<std::string> sgi_search_paths;

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--emit-ir") {
      emit_ir = true;
    } else if (arg == "--dump-ir") {
      dump_ir = true;
    } else if (arg == "--emit-obj") {
      emit_obj = true;
    } else if (arg == "--lib") {
      lib_mode = true;
    } else if (arg == "--sgi-path" && i + 1 < argc) {
      sgi_search_paths.push_back(argv[++i]);
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
      std::string stem = entry.path().stem().string();
      if (entry.is_regular_file() && entry.path().extension() == ".sg" &&
          (!is_platform_file(stem) || stem.ends_with(current_platform()))) {
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
  for (auto &sp : sgi_search_paths) {
    analyzer.package_resolver->sgi_search_paths.push_back(sp);
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

  // When --emit-obj or --lib: write .o to the -o path (default: <name>.o).
  // Otherwise:                 write a temp .o, link, then remove the temp.
  std::string obj_path;
  if (emit_obj || lib_mode) {
    obj_path = output_path.empty() ? (module_name + ".o") : output_path;
  } else {
    obj_path = module_name + ".o";
  }

  if (!codegen.write_object(obj_path)) {
    std::cerr << "Error: failed to emit object file\n";
    return 1;
  }

  if (emit_obj) {
    return 0;
  }

  // ── Library mode: write .o + .sgi, no link ─────────────────────────

  if (lib_mode) {
    // Generate .sgi interface file from the analyzer's package scope.
    std::vector<mc::SgiExport> exports;
    std::vector<mc::SgiImport> imports;

    if (analyzer.package_scope_) {
      for (auto &[sym_name, sym] : analyzer.package_scope_->symbols) {
        if (sym.is_public && !sym.is_builtin && sym.type) {
          bool is_type = (sym.kind == mc::SymbolKind::Type);
          exports.push_back({"", sym_name, sym.type, is_type});
        }
      }
    }

    // Determine .sgi output path alongside the .o.
    std::string sgi_base;
    if (!output_path.empty()) {
      // Strip extension from -o path to get base.
      fs::path op(output_path);
      sgi_base = (op.parent_path() / op.stem()).string();
    } else {
      sgi_base = module_name;
    }
    std::string sgi_path = sgi_base + ".sgi";

    if (!mc::write_sgi(sgi_path, module_name, imports, exports)) {
      std::cerr << std::format("Error: cannot write interface to '{}'\n",
                               sgi_path);
      return 1;
    }
    return 0;
  }

  // Link with the system linker (cc), including the Saga runtime.
#ifndef SAGA_RUNTIME_LIB
#define SAGA_RUNTIME_LIB ""
#endif
  std::string binary_path = output_path.empty() ? "a.out" : output_path;
  std::string runtime_lib = SAGA_RUNTIME_LIB;

  // Collect dependency .o files from .sgi-resolved imports.
  std::string dep_objects;
  for (auto &[imp_path, dir] : analyzer.package_resolver->sgi_resolved_dirs) {
    // Extract package name (last segment of import path).
    auto last_slash = imp_path.rfind('/');
    std::string pkg_name = (last_slash != std::string::npos)
                               ? imp_path.substr(last_slash + 1)
                               : imp_path;
    std::string dep_obj = dir + "/" + pkg_name + ".o";
    if (fs::is_regular_file(dep_obj)) {
      dep_objects += " " + dep_obj;
    }
  }

  std::string link_cmd =
      std::format("cc {} {}{} -o {} -no-pie", obj_path, runtime_lib,
                  dep_objects, binary_path);
  int link_status = std::system(link_cmd.c_str());

  // Clean up the temporary object file.
  std::filesystem::remove(obj_path);

  if (link_status != 0) {
    std::cerr << "Error: linking failed\n";
    return 1;
  }

  return 0;
}

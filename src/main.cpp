// Copyright 2026 Rob Thornton
// SPDX-License-Identifier: MIT

#include "build/build_graph.hpp"
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

// ---------------------------------------------------------------------------
// Collect all platform-filtered .sg files from a directory.
// Returns sorted list.
// ---------------------------------------------------------------------------

static std::vector<fs::path> collect_sg_files(const fs::path &dir) {
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
// compile_package — compile one package to .o + .sgi (library mode).
//
// Searches dep_sgi_dirs for .sgi files of already-compiled dependencies.
// Writes artifacts to output_dir/<pkg_name>.o and output_dir/<pkg_name>.sgi.
// Returns false and prints an error on failure.
// ---------------------------------------------------------------------------

static bool compile_package(const std::string &source_dir,
                             const std::string &pkg_name,
                             const std::string &output_dir,
                             const std::vector<std::string> &search_paths,
                             const std::vector<std::string> &sgi_dirs,
                             bool verbose) {
  // Ensure output dir exists.
  std::error_code ec;
  fs::create_directories(output_dir, ec);
  if (ec) {
    std::cerr << std::format("Error: cannot create output dir '{}': {}\n",
                             output_dir, ec.message());
    return false;
  }

  // ── Load source files ───────────────────────────────────────────────

  mc::FileSet fileset;
  auto sg_files = collect_sg_files(source_dir);
  if (sg_files.empty()) {
    std::cerr << std::format("Error: no .sg files in '{}'\n", source_dir);
    return false;
  }
  for (auto &sg : sg_files) {
    auto file = mc::File::from_path(sg.string());
    if (!file) {
      std::cerr << std::format("Error: cannot open '{}'\n", sg.string());
      return false;
    }
    fileset.add_file(std::move(file));
  }

  // ── Parse ───────────────────────────────────────────────────────────

  mc::Parser parser(fileset);
  auto ast = parser.parse();
  if (!ast || !parser.errors.errors.empty()) {
    if (!parser.errors.errors.empty())
      parser.errors.print_errors();
    else
      std::cerr << "Error: parse failed\n";
    return false;
  }

  // ── Analyze ─────────────────────────────────────────────────────────

  mc::Analyzer analyzer(fileset);
  analyzer.current_package_dir = source_dir;

  // Include parent dir for sibling package resolution.
  fs::path parent = fs::path(source_dir).parent_path();
  if (!parent.empty())
    analyzer.package_resolver->search_paths.push_back(parent.string());
  for (auto &sp : search_paths)
    analyzer.package_resolver->search_paths.push_back(sp);
  for (auto &sd : sgi_dirs)
    analyzer.package_resolver->sgi_search_paths.push_back(sd);

  analyzer.analyze(*ast);
  if (!analyzer.errors.errors.empty()) {
    analyzer.errors.print_errors();
    return false;
  }

  // ── Codegen ─────────────────────────────────────────────────────────

  mc::CodeGen codegen(pkg_name, analyzer);
  codegen.emit(*ast);

  // ── Write .o ────────────────────────────────────────────────────────

  std::string obj_path = output_dir + "/" + pkg_name + ".o";
  if (!codegen.write_object(obj_path)) {
    std::cerr << std::format("Error: failed to write '{}'\n", obj_path);
    return false;
  }

  // ── Write .sgi ──────────────────────────────────────────────────────

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

  std::string sgi_path = output_dir + "/" + pkg_name + ".sgi";
  if (!mc::write_sgi(sgi_path, pkg_name, imports, exports)) {
    std::cerr << std::format("Error: cannot write interface to '{}'\n",
                             sgi_path);
    return false;
  }

  if (verbose)
    std::cerr << std::format("  compiled {} → {}/{}.{{o,sgi}}\n",
                             pkg_name, output_dir, pkg_name);
  return true;
}

// ---------------------------------------------------------------------------
// build_graph_mode — compile all packages in dependency order.
//
// For each dep node (all but root) in topological order:
//   - skip if up-to-date (needs_rebuild() == false)
//   - compile to output_dir/<name>/{<name>.o, <name>.sgi}
//   - save content hash
//
// For the root node:
//   - if lib_mode: compile to output_dir/<root_name>.{o,sgi}
//   - else: compile then link all dep .o files into a binary
//
// Returns exit code (0 = success).
// ---------------------------------------------------------------------------

static int build_graph_mode(const std::string &source_dir,
                             const std::string &import_path,
                             const std::string &output_dir,
                             const std::vector<std::string> &search_paths,
                             const std::string &binary_path,
                             bool lib_mode,
                             bool verbose) {
  mc::BuildGraph graph;
  if (!graph.scan(source_dir, import_path, output_dir, search_paths)) {
    std::cerr << std::format("Error: {}\n", graph.error);
    return 1;
  }

  auto sorted = graph.sorted();
  if (sorted.empty() && !graph.error.empty()) {
    std::cerr << std::format("Error: {}\n", graph.error);
    return 1;
  }

  if (verbose)
    std::cerr << std::format("Build graph: {} package(s)\n", sorted.size());

  // Collect sgi dirs for each node as we compile them, in order.
  // These let downstream packages find the .sgi of already-built deps.
  std::vector<std::string> cumulative_sgi_dirs;

  std::vector<std::string> dep_obj_paths;   // for final link
  std::vector<std::string> dep_sgi_dirs;    // for final package's analyzer

  for (size_t i = 0; i < sorted.size(); ++i) {
    const auto *node = sorted[i];
    bool is_root = (i + 1 == sorted.size());

    if (mc::BuildGraph::needs_rebuild(*node)) {
      if (verbose)
        std::cerr << std::format("  building {}\n", node->import_path);

      if (!compile_package(node->source_dir, node->name,
                           node->output_dir, search_paths,
                           cumulative_sgi_dirs, verbose))
        return 1;

      if (!mc::BuildGraph::save_hash(*node)) {
        // Non-fatal — just means next build won't be incremental.
        std::cerr << std::format(
            "Warning: could not save hash for '{}'\n", node->import_path);
      }
    } else if (verbose) {
      std::cerr << std::format("  up-to-date {}\n", node->import_path);
    }

    // Dep nodes' output dirs feed into subsequent packages' sgi search.
    cumulative_sgi_dirs.push_back(node->output_dir);

    if (!is_root) {
      std::string obj = node->output_dir + "/" + node->name + ".o";
      if (fs::is_regular_file(obj))
        dep_obj_paths.push_back(obj);
      dep_sgi_dirs.push_back(node->output_dir);
    }
  }

  if (lib_mode) {
    // Root is a library — already compiled above; nothing more to do.
    return 0;
  }

  // ── Link binary ─────────────────────────────────────────────────────

  if (sorted.empty())
    return 0;

  const auto *root = sorted.back();
  std::string root_obj = root->output_dir + "/" + root->name + ".o";

  std::string dep_objs;
  for (auto &obj : dep_obj_paths)
    dep_objs += " " + obj;

#ifndef SAGA_RUNTIME_LIB
#define SAGA_RUNTIME_LIB ""
#endif
  std::string runtime_lib = SAGA_RUNTIME_LIB;
  std::string link_cmd = std::format("cc {} {}{} -o {} -no-pie",
                                     root_obj, runtime_lib, dep_objs,
                                     binary_path);
  if (verbose)
    std::cerr << std::format("  link: {}\n", link_cmd);

  int link_status = std::system(link_cmd.c_str());
  if (link_status != 0) {
    std::cerr << "Error: linking failed\n";
    return 1;
  }
  return 0;
}

// ---------------------------------------------------------------------------
// usage
// ---------------------------------------------------------------------------

static void usage(const char *prog) {
  std::cerr << std::format("Usage: {} [options] <source.sg | directory>\n",
                           prog);
  std::cerr << "Options:\n";
  std::cerr << "  --emit-ir         Write LLVM IR to <name>.ll\n";
  std::cerr << "  --emit-obj        Write package to object file\n";
  std::cerr << "  --lib             Build as library (.o + .sgi, no link)\n";
  std::cerr << "  --build           Build using the dependency graph (incremental)\n";
  std::cerr << "  --dump-ir         Print LLVM IR to stderr\n";
  std::cerr << "  -o <file>         Output file name (default: a.out)\n";
  std::cerr << "  -I <dir>          Add directory to package search path\n";
  std::cerr << "  --sgi-path <dir>  Add directory to .sgi search path\n";
  std::cerr << "  --out-dir <dir>   Output directory for --build mode\n";
  std::cerr << "  -v                Verbose build output\n";
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char **argv) {
  // ── Parse CLI arguments ────────────────────────────────────────────

  std::string source_path;
  std::string output_path;
  std::string out_dir;
  bool emit_ir = false;
  bool dump_ir = false;
  bool emit_obj = false;
  bool lib_mode = false;
  bool build_mode = false;
  bool verbose = false;
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
    } else if (arg == "--build") {
      build_mode = true;
    } else if (arg == "-v") {
      verbose = true;
    } else if (arg == "--sgi-path" && i + 1 < argc) {
      sgi_search_paths.push_back(argv[++i]);
    } else if (arg == "--out-dir" && i + 1 < argc) {
      out_dir = argv[++i];
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

  // ── Build graph mode ───────────────────────────────────────────────
  // --build: scan the dep graph, compile in order, link.

  if (build_mode) {
    fs::path input(source_path);
    if (!fs::is_directory(input)) {
      std::cerr << "Error: --build requires a package directory\n";
      return 1;
    }

    std::string abs_source = fs::canonical(input).string();
    std::string pkg_name = input.filename().string();

    // Default output dir: <source>/.build or -o dir.
    std::string build_out_dir = out_dir.empty()
        ? (abs_source + "/.build")
        : out_dir;

    // The final binary path.
    std::string bin_path = output_path.empty() ? "a.out" : output_path;

    return build_graph_mode(abs_source, pkg_name, build_out_dir,
                            search_paths, bin_path, lib_mode, verbose);
  }

  // ── Legacy single-package mode ─────────────────────────────────────
  // Unchanged from previous behavior.

  mc::FileSet fileset;

  fs::path input_path(source_path);
  std::string package_dir;

  if (fs::is_directory(input_path)) {
    package_dir = fs::canonical(input_path).string();
    auto sg_files = collect_sg_files(input_path);
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

  mc::Analyzer analyzer(fileset);
  if (!package_dir.empty()) {
    analyzer.current_package_dir = package_dir;
    fs::path parent = fs::path(package_dir).parent_path();
    if (!parent.empty())
      analyzer.package_resolver->search_paths.push_back(parent.string());
  }
  for (auto &sp : search_paths)
    analyzer.package_resolver->search_paths.push_back(sp);
  for (auto &sp : sgi_search_paths)
    analyzer.package_resolver->sgi_search_paths.push_back(sp);

  analyzer.analyze(*ast);
  if (!analyzer.errors.errors.empty()) {
    analyzer.errors.print_errors();
    return 1;
  }

  std::string module_name;
  if (fs::is_directory(input_path))
    module_name = fs::path(source_path).filename().string();
  else
    module_name = fs::path(source_path).stem().string();

  mc::CodeGen codegen(module_name, analyzer);
  codegen.emit(*ast);

  if (dump_ir)
    codegen.dump();

  if (emit_ir) {
    std::string ir_path = module_name + ".ll";
    if (!codegen.write_ir(ir_path)) {
      std::cerr << std::format("Error: cannot write IR to '{}'\n", ir_path);
      return 1;
    }
    std::cerr << std::format("Wrote {}\n", ir_path);
    return 0;
  }

  std::string obj_path;
  if (emit_obj || lib_mode)
    obj_path = output_path.empty() ? (module_name + ".o") : output_path;
  else
    obj_path = module_name + ".o";

  if (!codegen.write_object(obj_path)) {
    std::cerr << "Error: failed to emit object file\n";
    return 1;
  }

  if (emit_obj)
    return 0;

  if (lib_mode) {
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
    std::string sgi_base;
    if (!output_path.empty()) {
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

#ifndef SAGA_RUNTIME_LIB
#define SAGA_RUNTIME_LIB ""
#endif
  std::string binary_path = output_path.empty() ? "a.out" : output_path;
  std::string runtime_lib = SAGA_RUNTIME_LIB;

  std::string dep_objects;
  for (auto &[imp_path, dir] : analyzer.package_resolver->sgi_resolved_dirs) {
    auto last_slash = imp_path.rfind('/');
    std::string pkg_name = (last_slash != std::string::npos)
                               ? imp_path.substr(last_slash + 1)
                               : imp_path;
    std::string dep_obj = dir + "/" + pkg_name + ".o";
    if (fs::is_regular_file(dep_obj))
      dep_objects += " " + dep_obj;
  }

  std::string link_cmd =
      std::format("cc {} {}{} -o {} -no-pie", obj_path, runtime_lib,
                  dep_objects, binary_path);
  int link_status = std::system(link_cmd.c_str());
  std::filesystem::remove(obj_path);

  if (link_status != 0) {
    std::cerr << "Error: linking failed\n";
    return 1;
  }
  return 0;
}

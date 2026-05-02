// Copyright 2026 Rob Thornton
// SPDX-License-Identifier: MIT

#include "cmd/cmd.hpp"
#include "cmd/common.hpp"

#include "build/build_graph.hpp"
#include "frontend/fileset.hpp"
#include "frontend/parser.hpp"
#include "ir/codegen.hpp"
#include "semantic/analyzer.hpp"
#include "semantic/sgi.hpp"

#include <algorithm>
#include <filesystem>
#include <format>
#include <iostream>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// compile_package — compile one package to .o + .sgi (library mode).
//
// Used by both the --lib single-package path and the --build DAG path.
// ---------------------------------------------------------------------------

/// Collect receiver methods from a stdlib analyzer into SgiReceiverMethod vec.
static std::vector<saga::SgiReceiverMethod>
collect_receiver_methods(const saga::Analyzer &analyzer) {
  std::vector<saga::SgiReceiverMethod> result;
  // Intrinsic scalar types (Int, Float, Bool, String) — keyed by Type*.
  for (auto &[type_ptr, methods] : analyzer.type_methods_) {
    std::string type_name;
    switch (type_ptr->kind) {
    case saga::TypeKind::Int: {
      auto &ii = std::get<saga::IntType>(type_ptr->detail);
      if (ii.bits == 0)
        type_name = "Int";
      else if (ii.is_signed)
        type_name = "Int" + std::to_string(ii.bits);
      else
        type_name = "Uint" + std::to_string(ii.bits);
      break;
    }
    case saga::TypeKind::Float: {
      auto &fi = std::get<saga::FloatType>(type_ptr->detail);
      if (fi.bits == 0)
        type_name = "Float";
      else
        type_name = "Float" + std::to_string(fi.bits);
      break;
    }
    case saga::TypeKind::Bool:   type_name = "Bool";   break;
    case saga::TypeKind::String: type_name = "String"; break;
    default: break;
    }
    if (type_name.empty())
      continue;
    std::vector<saga::MethodInfo> pub;
    for (auto &m : methods)
      if (m.is_public) pub.push_back(m);
    if (!pub.empty())
      result.push_back({type_name, std::move(pub)});
  }
  // Generic types (Array, Map) — keyed by TypeKind.
  for (auto &[kind, methods] : analyzer.kind_methods_) {
    std::string type_name;
    switch (kind) {
    case saga::TypeKind::Array: type_name = "Array"; break;
    case saga::TypeKind::Map:   type_name = "Map";   break;
    default: break;
    }
    if (type_name.empty())
      continue;
    std::vector<saga::MethodInfo> pub;
    for (auto &m : methods)
      if (m.is_public) pub.push_back(m);
    if (!pub.empty())
      result.push_back({type_name, std::move(pub)});
  }
  return result;
}

static bool compile_package(const std::string &source_dir,
                             const std::string &pkg_name,
                             const std::string &output_dir,
                             const std::vector<std::string> &search_paths,
                             const std::vector<std::string> &sgi_dirs,
                             const std::vector<std::string> &init_symbols,
                             bool verbose,
                             bool stdlib_mode = false) {
  std::error_code ec;
  fs::create_directories(output_dir, ec);
  if (ec) {
    std::cerr << std::format("Error: cannot create output dir '{}': {}\n",
                             output_dir, ec.message());
    return false;
  }

  saga::FileSet fileset;
  auto sg_files = collect_sg_files(source_dir);
  if (sg_files.empty()) {
    std::cerr << std::format("Error: no .sg files in '{}'\n", source_dir);
    return false;
  }
  for (auto &sg : sg_files) {
    auto file = saga::File::from_path(sg.string());
    if (!file) {
      std::cerr << std::format("Error: cannot open '{}'\n", sg.string());
      return false;
    }
    fileset.add_file(std::move(file));
  }

  saga::Parser parser(fileset);
  auto ast = parser.parse();
  if (!ast || !parser.errors.errors.empty()) {
    if (!parser.errors.errors.empty())
      parser.errors.print_errors();
    else
      std::cerr << "Error: parse failed\n";
    return false;
  }

  saga::Analyzer analyzer(fileset);
  analyzer.current_package_dir = source_dir;
  analyzer.is_stdlib = stdlib_mode;
  {
    fs::path parent = fs::path(source_dir).parent_path();
    if (!parent.empty())
      analyzer.package_resolver->search_paths.push_back(parent.string());
  }
  for (auto &sp : search_paths)
    analyzer.package_resolver->search_paths.push_back(sp);
  for (auto &sd : sgi_dirs)
    analyzer.package_resolver->sgi_search_paths.push_back(sd);

  analyzer.analyze(*ast);
  if (!analyzer.errors.errors.empty()) {
    analyzer.errors.print_errors();
    return false;
  }

  saga::CodeGen codegen(pkg_name, analyzer);
  codegen.imported_init_symbols = init_symbols;
  codegen.emit(*ast);

  std::string obj_path = output_dir + "/" + pkg_name + ".o";
  if (!codegen.write_object(obj_path)) {
    std::cerr << std::format("Error: failed to write '{}'\n", obj_path);
    return false;
  }

  std::vector<saga::SgiExport> exports;
  std::vector<saga::SgiImport> imports;
  if (analyzer.package_scope_) {
    for (auto &[sym_name, sym] : analyzer.package_scope_->symbols) {
      if (sym.is_public && !sym.is_builtin && sym.type) {
        bool is_type = (sym.kind == saga::SymbolKind::Type);
        std::string origin = saga::origin_of(sym.type);
        if (origin.empty()) origin = pkg_name;
        exports.push_back({"", sym_name, sym.type, is_type, origin});
      }
    }
  }
  auto receiver_methods = collect_receiver_methods(analyzer);
  std::string sgi_path = output_dir + "/" + pkg_name + ".sgi";
  std::error_code abs_ec;
  std::string abs_source_dir =
      fs::absolute(source_dir, abs_ec).lexically_normal().string();
  if (abs_ec) abs_source_dir.clear();
  if (!saga::write_sgi(sgi_path, pkg_name, imports, exports, receiver_methods,
                     abs_source_dir)) {
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
// ---------------------------------------------------------------------------

static int build_graph_mode(const char *prog,
                             const std::string &source_dir,
                             const std::string &import_path,
                             const std::string &output_dir,
                             const std::vector<std::string> &search_paths,
                             const std::vector<std::string> &sgi_search_paths,
                             const std::string &binary_path,
                             bool lib_mode,
                             bool verbose) {
  saga::BuildGraph graph;
  if (!graph.scan(source_dir, import_path, output_dir, search_paths,
                  sgi_search_paths)) {
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

  // Seed cumulative sgi dirs with stdlib + user-supplied paths.
  std::vector<std::string> cumulative_sgi_dirs = sgi_search_paths;
  {
    std::string std_sgi = saga_std_sgi_dir(prog);
    if (fs::is_directory(std_sgi) &&
        std::find(cumulative_sgi_dirs.begin(), cumulative_sgi_dirs.end(),
                  std_sgi) == cumulative_sgi_dirs.end())
      cumulative_sgi_dirs.push_back(std_sgi);
  }

  std::vector<std::string> dep_obj_paths;

  // Track which packages need a runtime `<pkg>__init__` so the binary's
  // Main wrapper can call them in topological order.  Keyed by import path.
  std::unordered_map<std::string, bool> pkg_needs_init;
  std::unordered_map<std::string, std::string> pkg_link_name;

  // Index nodes by import path for transitive-dep walks.
  std::unordered_map<std::string, const saga::BuildGraph::Node *> by_path;
  for (const auto *n : sorted)
    by_path[n->import_path] = n;

  for (size_t i = 0; i < sorted.size(); ++i) {
    const auto *node = sorted[i];
    bool is_root = (i + 1 == sorted.size());

    // Walk transitive deps in topological order.  `sorted` is already
    // dep-first, so iterating prefixes preserves ordering; we filter to
    // the transitive closure of node->deps.
    std::unordered_set<std::string> trans_deps;
    {
      std::vector<std::string> stack(node->deps.begin(), node->deps.end());
      while (!stack.empty()) {
        std::string cur = std::move(stack.back());
        stack.pop_back();
        if (!trans_deps.insert(cur).second)
          continue;
        auto it = by_path.find(cur);
        if (it != by_path.end())
          for (auto &d : it->second->deps) stack.push_back(d);
      }
    }
    std::vector<std::string> init_symbols;
    for (size_t j = 0; j < i; ++j) {
      const auto *dep = sorted[j];
      if (!trans_deps.count(dep->import_path))
        continue;
      if (pkg_needs_init.count(dep->import_path) &&
          pkg_needs_init[dep->import_path])
        init_symbols.push_back(pkg_link_name[dep->import_path]);
    }

    if (saga::BuildGraph::needs_rebuild(*node)) {
      if (verbose)
        std::cerr << std::format("  building {}\n", node->import_path);
      if (!compile_package(node->source_dir, node->name, node->output_dir,
                           search_paths, cumulative_sgi_dirs, init_symbols,
                           verbose))
        return 1;
      if (!saga::BuildGraph::save_hash(*node))
        std::cerr << std::format("Warning: could not save hash for '{}'\n",
                                 node->import_path);
    } else if (verbose) {
      std::cerr << std::format("  up-to-date {}\n", node->import_path);
    }

    // Record whether this freshly-compiled-or-cached package needs init.
    {
      std::string sgi = node->output_dir + "/" + node->name + ".sgi";
      auto parsed = saga::load_sgi(sgi);
      pkg_needs_init[node->import_path] =
          parsed && saga::sgi_needs_init(*parsed);
      pkg_link_name[node->import_path] = node->name + "__init__";
    }

    cumulative_sgi_dirs.push_back(node->output_dir);

    if (!is_root) {
      std::string obj = node->output_dir + "/" + node->name + ".o";
      if (fs::is_regular_file(obj))
        dep_obj_paths.push_back(obj);
    }
  }

  if (lib_mode)
    return 0;

  if (sorted.empty())
    return 0;

  const auto *root = sorted.back();
  std::string root_obj = root->output_dir + "/" + root->name + ".o";

  std::string dep_objs;
  for (auto &obj : dep_obj_paths)
    dep_objs += " " + obj;

  std::string runtime_lib = saga_runtime_lib();
  std::string std_lib_path = saga_std_lib(prog);
  std::string std_lib_arg =
      fs::is_regular_file(std_lib_path) ? " " + std_lib_path : "";

  std::string link_cmd =
      std::format("cc {} -Wl,--start-group{} {} {} -Wl,--end-group -o {} -no-pie",
                  root_obj, std_lib_arg, dep_objs, runtime_lib, binary_path);
  if (verbose)
    std::cerr << std::format("  link: {}\n", link_cmd);

  if (std::system(link_cmd.c_str()) != 0) {
    std::cerr << "Error: linking failed\n";
    return 1;
  }
  return 0;
}

// ---------------------------------------------------------------------------
// cmd_build
// ---------------------------------------------------------------------------

static void usage_build() {
  std::cerr << "Usage: saga build [options] <source.sg | directory>\n"
               "       saga [options] <source.sg | directory>   (legacy)\n"
               "\n"
               "Options:\n"
               "  --lib             Build as library (.o + .sgi, no link)\n"
               "  --stdlib          Mark package as stdlib (enables intrinsics)\n"
               "  --build           Build using the dependency graph (incremental)\n"
               "  --emit-obj        Write package to object file only\n"
               "  --emit-ir         Write LLVM IR to <name>.ll\n"
               "  --dump-ir         Print LLVM IR to stderr\n"
               "  -o <file>         Output file name (default: a.out)\n"
               "  -I <dir>          Add directory to package search path\n"
               "  --sgi-path <dir>  Add directory to .sgi search path\n"
               "  --out-dir <dir>   Output directory for --build mode\n"
               "  -v                Verbose output\n";
}

int cmd_build(const char *prog, int argc, char **argv) {
  std::string source_path;
  std::string output_path;
  std::string out_dir;
  bool emit_ir = false;
  bool dump_ir = false;
  bool emit_obj = false;
  bool lib_mode = false;
  bool dag_mode = false;
  bool stdlib_mode = false;
  bool verbose = false;
  std::vector<std::string> search_paths;
  std::vector<std::string> sgi_search_paths;

  // Argument parsing starts from argv[1] (argv[0] is "build" or the binary).
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--emit-ir")          { emit_ir = true; }
    else if (arg == "--dump-ir")     { dump_ir = true; }
    else if (arg == "--emit-obj")    { emit_obj = true; }
    else if (arg == "--lib")         { lib_mode = true; }
    else if (arg == "--stdlib")      { stdlib_mode = true; }
    else if (arg == "--build")       { dag_mode = true; }
    else if (arg == "-v")            { verbose = true; }
    else if (arg == "--sgi-path" && i + 1 < argc)
      sgi_search_paths.push_back(argv[++i]);
    else if (arg == "--out-dir" && i + 1 < argc)
      out_dir = argv[++i];
    else if (arg == "-o" && i + 1 < argc)
      output_path = argv[++i];
    else if (arg == "-I" && i + 1 < argc)
      search_paths.push_back(argv[++i]);
    else if (arg[0] == '-') {
      std::cerr << std::format("Unknown option: {}\n", arg);
      usage_build();
      return 1;
    } else {
      source_path = arg;
    }
  }

  if (source_path.empty()) {
    usage_build();
    return 1;
  }

  // Apply deps from the nearest project.saga (local paths + cached remotes).
  apply_manifest_deps(prog, fs::current_path().string(), search_paths,
                      sgi_search_paths);

  // ── DAG mode ──────────────────────────────────────────────────────────────

  if (dag_mode) {
    fs::path input(source_path);
    if (!fs::is_directory(input)) {
      std::cerr << "Error: --build requires a package directory\n";
      return 1;
    }
    std::string abs_source = fs::canonical(input).string();
    std::string pkg_name = input.filename().string();
    std::string build_out = out_dir.empty() ? (abs_source + "/.build") : out_dir;
    std::string bin_path = output_path.empty() ? "a.out" : output_path;

    // Auto-add stdlib SGI dir.
    std::string std_sgi = saga_std_sgi_dir(prog);
    if (fs::is_directory(std_sgi) &&
        std::find(sgi_search_paths.begin(), sgi_search_paths.end(), std_sgi)
            == sgi_search_paths.end())
      sgi_search_paths.push_back(std_sgi);

    return build_graph_mode(prog, abs_source, pkg_name, build_out,
                            search_paths, sgi_search_paths, bin_path,
                            lib_mode, verbose);
  }

  // ── Single-package mode ───────────────────────────────────────────────────

  saga::FileSet fileset;
  std::string package_dir = load_sources(source_path, fileset);
  if (package_dir.empty() && fileset.files.empty())
    return 1;

  saga::Parser parser(fileset);
  auto ast = parser.parse();
  if (!ast) { std::cerr << "Error: parse failed\n"; return 1; }
  if (!parser.errors.errors.empty()) { parser.errors.print_errors(); return 1; }

  saga::Analyzer analyzer(fileset);
  analyzer.is_stdlib = stdlib_mode;
  setup_analyzer_paths(analyzer, package_dir, search_paths, sgi_search_paths,
                       prog);
  analyzer.analyze(*ast);
  if (!analyzer.errors.errors.empty()) { analyzer.errors.print_errors(); return 1; }

  fs::path input_path(source_path);
  std::string module_name = fs::is_directory(input_path)
      ? input_path.filename().string()
      : input_path.stem().string();

  saga::CodeGen codegen(module_name, analyzer);
  // Best-effort init-symbol discovery: each resolved import's .sgi tells us
  // whether that package owns runtime-allocated consts.  Single-package mode
  // has no full build graph, so order falls back to map-iteration order;
  // unrelated packages can init in any order without observable difference.
  for (auto &[imp_path, dir] : analyzer.package_resolver->sgi_resolved_dirs) {
    auto slash = imp_path.rfind('/');
    std::string pname = (slash != std::string::npos)
                            ? imp_path.substr(slash + 1) : imp_path;
    std::string sgi = dir + "/" + pname + ".sgi";
    auto parsed = saga::load_sgi(sgi);
    if (parsed && saga::sgi_needs_init(*parsed))
      codegen.imported_init_symbols.push_back(pname + "__init__");
  }
  codegen.emit(*ast);

  if (dump_ir) codegen.dump();

  if (emit_ir) {
    std::string ir_path = module_name + ".ll";
    if (!codegen.write_ir(ir_path)) {
      std::cerr << std::format("Error: cannot write IR to '{}'\n", ir_path);
      return 1;
    }
    std::cerr << std::format("Wrote {}\n", ir_path);
    return 0;
  }

  std::string obj_path = (emit_obj || lib_mode)
      ? (output_path.empty() ? module_name + ".o" : output_path)
      : module_name + ".o";

  if (!codegen.write_object(obj_path)) {
    std::cerr << "Error: failed to emit object file\n";
    return 1;
  }
  if (emit_obj) return 0;

  if (lib_mode) {
    std::vector<saga::SgiExport> exports;
    std::vector<saga::SgiImport> imports;
    if (analyzer.package_scope_) {
      for (auto &[sym_name, sym] : analyzer.package_scope_->symbols) {
        if (sym.is_public && !sym.is_builtin && sym.type) {
          bool is_type = (sym.kind == saga::SymbolKind::Type);
          std::string origin = saga::origin_of(sym.type);
          if (origin.empty()) origin = module_name;
          exports.push_back({"", sym_name, sym.type, is_type, origin});
        }
      }
    }
    auto receiver_methods = collect_receiver_methods(analyzer);
    fs::path op(obj_path);
    std::string sgi_path = (op.parent_path() / op.stem()).string() + ".sgi";
    std::string abs_source_dir;
    if (!package_dir.empty()) {
      std::error_code abs_ec;
      abs_source_dir =
          fs::absolute(package_dir, abs_ec).lexically_normal().string();
      if (abs_ec) abs_source_dir.clear();
    }
    if (!saga::write_sgi(sgi_path, module_name, imports, exports,
                       receiver_methods, abs_source_dir)) {
      std::cerr << std::format("Error: cannot write interface to '{}'\n",
                               sgi_path);
      return 1;
    }
    return 0;
  }

  // ── Link ──────────────────────────────────────────────────────────────────

  std::string binary_path = output_path.empty() ? "a.out" : output_path;
  std::string runtime_lib = saga_runtime_lib();
  std::string std_lib_path = saga_std_lib(prog);
  std::string std_lib_arg =
      fs::is_regular_file(std_lib_path) ? " " + std_lib_path : "";

  // Dep .o from user packages resolved via .sgi (skip stdlib dir).
  std::string dep_objects;
  std::string std_sgi_dir = saga_std_sgi_dir(prog);
  for (auto &[imp_path, dir] : analyzer.package_resolver->sgi_resolved_dirs) {
    if (!std_lib_arg.empty() && dir == std_sgi_dir)
      continue;
    auto slash = imp_path.rfind('/');
    std::string pname = slash != std::string::npos
                            ? imp_path.substr(slash + 1)
                            : imp_path;
    std::string dep_obj = dir + "/" + pname + ".o";
    if (fs::is_regular_file(dep_obj))
      dep_objects += " " + dep_obj;
  }

  std::string link_cmd = std::format("cc {} -Wl,--start-group{} {} {} -Wl,--end-group -o {} -no-pie",
                                     obj_path, std_lib_arg, dep_objects,
                                     runtime_lib, binary_path);
  int status = std::system(link_cmd.c_str());
  fs::remove(obj_path);
  if (status != 0) { std::cerr << "Error: linking failed\n"; return 1; }
  return 0;
}

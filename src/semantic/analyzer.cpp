// Copywrite 2026 Rob Thornton
// SPDX-License-Identifier: MIT

#include "semantic/analyzer.hpp"
#include "semantic/analyzer_detail.hpp"
#include "semantic/sgi.hpp"
#include "frontend/parser.hpp"
#include "util/internal_error.hpp"

#include <algorithm>
#include <charconv>
#include <filesystem>
#include <format>

namespace saga {

// ===========================================================================
// Construction
// ===========================================================================

Analyzer::Analyzer(FileSet &fs)
    : fileset(fs), global_scope(std::make_shared<Scope>(ScopeKind::Global)),
      current_scope(global_scope),
      package_resolver(std::make_shared<PackageResolver>()) {
  register_builtins(global_scope, builtins);
}

Analyzer::Analyzer(FileSet &fs, std::shared_ptr<PackageResolver> resolver)
    : fileset(fs), global_scope(std::make_shared<Scope>(ScopeKind::Global)),
      current_scope(global_scope), package_resolver(std::move(resolver)) {
  register_builtins(global_scope, builtins);
}

std::string Analyzer::current_package_name() const {
  if (!current_package_name_override.empty())
    return current_package_name_override;
  if (current_package_dir.empty()) return "";
  return std::filesystem::path(current_package_dir).filename().string();
}

// ===========================================================================
// PackageResolver
// ===========================================================================

namespace fs = std::filesystem;

std::string
PackageResolver::find_package_dir(const std::string &import_path) const {
  for (auto &base : search_paths) {
    std::string candidate = base + "/" + import_path;
    if (fs::is_directory(candidate))
      return candidate;
  }
  return {};
}

std::string
PackageResolver::find_sgi_file(const std::string &import_path) const {
  // Extract package name from import path (last segment).
  auto last_slash = import_path.rfind('/');
  std::string pkg_name = (last_slash != std::string::npos)
                             ? import_path.substr(last_slash + 1)
                             : import_path;
  std::string filename = pkg_name + ".sgi";

  for (auto &base : sgi_search_paths) {
    std::string candidate = base + "/" + filename;
    if (fs::is_regular_file(candidate))
      return candidate;
  }
  return {};
}

// Skip files like foo_darwin.sg when building for linux
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

std::vector<std::string>
PackageResolver::list_source_files(const std::string &dir) const {
  std::vector<std::string> files;
  if (!fs::is_directory(dir))
    return files;
  for (auto &entry : fs::directory_iterator(dir)) {
    std::string stem = entry.path().stem().string();
    if (entry.is_regular_file() && entry.path().extension() == ".sg" &&
        (!is_platform_file(stem) || stem.ends_with(current_platform()))) {
      files.push_back(entry.path().string());
    }
  }
  std::sort(files.begin(), files.end());
  return files;
}

// ===========================================================================
// Generic receiver method helpers
// ===========================================================================

/// Replace SGI stub types (Struct("T"), Struct("K"), Struct("V")) with the
/// sentinel TypeParam placeholders that check_selector's substitution expects.
/// Called when loading Array/Map receiver methods from a pre-compiled SGI file.
static TypePtr normalize_generic_receiver_sig(const TypePtr &t,
                                              TypeKind recv_kind) {
  if (!t)
    return t;

  // Replace a top-level stub.
  if (t->kind == TypeKind::Struct) {
    auto &sinfo = std::get<StructTypeInfo>(t->detail);
    if (sinfo.fields.empty() && sinfo.methods.empty()) {
      if (recv_kind == TypeKind::Array && sinfo.name == "T")
        return make_type_param(9990, "T");
      if (recv_kind == TypeKind::Map) {
        if (sinfo.name == "K") return make_type_param(9991, "K");
        if (sinfo.name == "V") return make_type_param(9992, "V");
      }
    }
  }

  // Recurse into compound types.
  switch (t->kind) {
  case TypeKind::Array: {
    auto &info = std::get<ArrayTypeInfo>(t->detail);
    auto elem = normalize_generic_receiver_sig(info.element, recv_kind);
    return (elem == info.element) ? t : make_array_type(std::move(elem));
  }
  case TypeKind::Map: {
    auto &info = std::get<MapTypeInfo>(t->detail);
    auto k = normalize_generic_receiver_sig(info.key, recv_kind);
    auto v = normalize_generic_receiver_sig(info.value, recv_kind);
    return (k == info.key && v == info.value) ? t
                                              : make_map_type(std::move(k), std::move(v));
  }
  case TypeKind::Func: {
    auto &info = std::get<FuncTypeInfo>(t->detail);
    bool changed = false;
    std::vector<TypePtr> params;
    for (auto &p : info.params) {
      auto np = normalize_generic_receiver_sig(p, recv_kind);
      if (np != p) changed = true;
      params.push_back(std::move(np));
    }
    TypePtr ret;
    if (info.return_type) {
      ret = normalize_generic_receiver_sig(info.return_type, recv_kind);
      if (ret != info.return_type) changed = true;
    }
    if (!changed) return t;
    auto result = make_func_type(std::move(params), std::move(ret));
    std::get<FuncTypeInfo>(result->detail).is_variadic = info.is_variadic;
    return result;
  }
  case TypeKind::Union: {
    auto &info = std::get<UnionTypeInfo>(t->detail);
    bool changed = false;
    std::vector<TypePtr> alts;
    for (auto &a : info.alternatives) {
      auto na = normalize_generic_receiver_sig(a, recv_kind);
      if (na != a) changed = true;
      alts.push_back(std::move(na));
    }
    return changed ? make_union_type(std::move(alts)) : t;
  }
  default: return t;
  }
}

// ===========================================================================
// Entry point
// ===========================================================================

void Analyzer::analyze(const Node &root) {
  std::visit(overloaded{
                 [&](const PackageNode &pkg) { visit_package(pkg); },
                 [&](const SourceNode &src) { visit_source(src); },
                 [&](const auto &) {
                   error(root.span, "expected package or source node");
                 },
             },
             root.data);
  report_deferred_bugs();
}

void Analyzer::load_prelude() {
  if (!package_resolver)
    return;

  // std/proto declares the named protocols the compiler dispatches
  // through (Hashable, Stringable).  Loaded for every analysis pass —
  // including stdlib leaf packages, whose generic bodies may dispatch
  // through these protocols on TypeParam values.  Skipped only when
  // proto itself is being compiled (proto.sgi doesn't exist yet).
  bool compiling_proto =
      is_stdlib && !current_package_dir.empty() &&
      std::filesystem::path(current_package_dir).filename().string() == "proto";
  if (!compiling_proto) {
    std::string proto_sgi = package_resolver->find_sgi_file("std/proto");
    if (!proto_sgi.empty()) {
      if (auto sgi = load_sgi(proto_sgi)) {
        for (auto &exp : sgi->exports) {
          if (!exp.type || exp.type->kind != TypeKind::Interface) continue;
          if (exp.name == "Hashable")   builtins.hashable_iface   = exp.type;
          if (exp.name == "Stringable") builtins.stringable_iface = exp.type;
        }
      }
    }
  }

  // Stdlib type packages (int, float, bool, string, array, map) define their
  // own receiver methods and must not load others' — otherwise they'd
  // re-export foreign methods in their SGI files.
  if (is_stdlib && !current_package_dir.empty()) {
    auto pkg = std::filesystem::path(current_package_dir).filename().string();
    static const char *type_pkgs[] = {
        "int", "float", "bool", "string", "array", "map"};
    for (auto *tp : type_pkgs)
      if (pkg == tp)
        return;
  }

  // --- Scalar types (Int, Float, Bool, String) ---
  static const std::string scalar_pkgs[] = {
    "std/int", "std/float", "std/bool", "std/string"
  };
  auto get_canonical = [this](const std::string &type_name) -> const Type * {
    if (type_name == "Int")    return builtins.int_type.get();
    if (type_name == "Int8")   return builtins.int8_type.get();
    if (type_name == "Int16")  return builtins.int16_type.get();
    if (type_name == "Int32")  return builtins.int32_type.get();
    if (type_name == "Int64")  return builtins.int64_type.get();
    if (type_name == "Uint8")  return builtins.uint8_type.get();
    if (type_name == "Uint16") return builtins.uint16_type.get();
    if (type_name == "Uint32") return builtins.uint32_type.get();
    if (type_name == "Uint64") return builtins.uint64_type.get();
    if (type_name == "Float")   return builtins.float_type.get();
    if (type_name == "Float32") return builtins.float32_type.get();
    if (type_name == "Float64") return builtins.float64_type.get();
    if (type_name == "Bool")    return builtins.bool_type.get();
    if (type_name == "String")  return builtins.string_type.get();
    return nullptr;
  };

  for (auto &pkg : scalar_pkgs) {
    std::string sgi_path = package_resolver->find_sgi_file(pkg);
    if (sgi_path.empty())
      continue;
    auto sgi = load_sgi(sgi_path);
    if (!sgi)
      continue;
    for (auto &rm : sgi->receiver_methods) {
      const Type *canonical = get_canonical(rm.type_name);
      if (!canonical)
        continue;
      for (auto &method : rm.methods) {
        // Avoid duplicate methods (idempotent if load_prelude called twice).
        auto &vec = type_methods_[canonical];
        bool dup = false;
        for (auto &existing : vec) {
          if (existing.name == method.name) { dup = true; break; }
        }
        if (!dup)
          vec.push_back(method);
      }
    }
  }

  // --- Generic types (Array, Map) ---
  static const std::string generic_pkgs[] = {"std/array", "std/map"};
  auto get_recv_kind = [](const std::string &tn) -> std::optional<TypeKind> {
    if (tn == "Array") return TypeKind::Array;
    if (tn == "Map")   return TypeKind::Map;
    return std::nullopt;
  };

  for (auto &pkg : generic_pkgs) {
    std::string sgi_path = package_resolver->find_sgi_file(pkg);
    if (sgi_path.empty())
      continue;
    auto sgi = load_sgi(sgi_path);
    if (!sgi)
      continue;
    if (!sgi->source_dir.empty()) {
      auto last_slash = pkg.rfind('/');
      std::string pkg_short = (last_slash != std::string::npos)
                                  ? pkg.substr(last_slash + 1)
                                  : pkg;
      package_resolver->source_dirs[pkg_short] = sgi->source_dir;
      package_resolver->source_dirs[pkg] = sgi->source_dir;
    }
    for (auto &rm : sgi->receiver_methods) {
      auto kind_opt = get_recv_kind(rm.type_name);
      if (!kind_opt)
        continue;
      auto kind = *kind_opt;
      for (auto &method : rm.methods) {
        // Normalize stub type params to sentinels (T→9990, K→9991, V→9992).
        auto sig = normalize_generic_receiver_sig(method.signature, kind);
        auto &vec = kind_methods_[kind];
        bool dup = false;
        for (auto &e : vec)
          if (e.name == method.name) { dup = true; break; }
        if (!dup)
          vec.push_back({method.name, sig, method.is_public});
      }
    }
  }
}

// ===========================================================================
// Scope helpers
// ===========================================================================

void Analyzer::push_scope(ScopeKind kind) {
  current_scope = current_scope->child(kind);
}

void Analyzer::pop_scope() {
  if (current_scope->parent) {
    current_scope = current_scope->parent;
  }
}

void Analyzer::pop_resolve_scope() {
  report_unread_locals(*current_scope);
  pop_scope();
}

void Analyzer::pop_module_scope() {
  report_unused_imports(*current_scope);
  pop_scope();
}

static std::vector<const Symbol *> unread_symbols(const Scope &scope,
                                                  SymbolKind kind) {
  std::vector<const Symbol *> unread;
  for (auto &[name, sym] : scope.symbols) {
    if (sym.kind == kind && !scope.read_names.contains(name))
      unread.push_back(&sym);
  }

  // `symbols` is unordered, so sort or the diagnostics come out in a
  // different order from one run to the next.
  std::sort(unread.begin(), unread.end(), [](const Symbol *a, const Symbol *b) {
    return a->decl_span.start < b->decl_span.start;
  });
  return unread;
}

void Analyzer::report_unread_locals(const Scope &scope) {
  if (suppress_unread_reports_)
    return;

  for (const Symbol *sym : unread_symbols(scope, SymbolKind::Variable))
    error(sym->decl_span,
          std::format("'{}' is declared but never read; remove it or name it "
                      "'_{}'",
                      sym->name, sym->name));
}

void Analyzer::report_unused_imports(const Scope &scope) {
  for (const Symbol *sym : unread_symbols(scope, SymbolKind::Module)) {
    // An import that failed to resolve already owns its line, and there the
    // fix is the path, not the import.
    if (!sym->type || is_invalid_type(sym->type))
      continue;
    error(sym->decl_span,
          std::format("package '{}' is imported but never used; remove the "
                      "import",
                      sym->name));
  }
}

bool Analyzer::declare(const Symbol &sym) {
  if (!current_scope->declare(sym)) {
    redeclaration_error(sym.decl_span, sym.name);
    return false;
  }
  return true;
}

bool Analyzer::declare_local(const Symbol &sym) {
  // An ignored name is not a binding, so there is nothing for a second one to
  // collide with and nothing for a nested one to shadow.
  if (is_ignored_name(sym.name))
    return true;

  // Check same-scope redeclaration.
  if (current_scope->lookup_local(sym.name)) {
    redeclaration_error(sym.decl_span, sym.name);
    return false;
  }
  // Check outer-scope shadowing (language rule: shadowing is an error).
  if (current_scope->parent && current_scope->parent->lookup(sym.name)) {
    shadowing_error(sym.decl_span, sym.name);
    return false;
  }
  current_scope->symbols.emplace(sym.name, sym);
  return true;
}

std::optional<Symbol> Analyzer::lookup(const std::string &name) const {
  return current_scope->lookup(name);
}

// ===========================================================================
// Type-parameter helpers
// ===========================================================================

uint32_t Analyzer::fresh_type_param_id() { return next_type_param_id++; }

TypePtr Analyzer::resolve_constraint_bound(const IdentifierNode &constraint) {
  if (auto sym = lookup(std::string(constraint.name))) {
    if (sym->type && sym->type->kind == TypeKind::Interface)
      return sym->type;
    error(constraint.span,
          std::format("'{}' is not an interface — a constraint must be an "
                      "interface or one of integer, float, numeric",
                      constraint.name));
    return nullptr;
  }
  if (constraint.name == "Stringable" && builtins.stringable_iface)
    return builtins.stringable_iface;
  if (constraint.name == "Hashable" && builtins.hashable_iface)
    return builtins.hashable_iface;
  error(constraint.span,
        std::format("unknown type constraint '{}' — expected an interface or "
                    "one of integer, float, numeric",
                    constraint.name));
  return nullptr;
}

std::vector<TypeParam> Analyzer::enter_generics(const GenericNode &generic) {
  std::vector<TypeParam> params;
  for (auto &tp_node : generic.type_params) {
    std::string_view name;
    Span span = tp_node->span;
    TypeConstraint constraint = TypeConstraint::None;
    TypePtr bound;

    if (auto *tp = std::get_if<TypeParamNode>(&tp_node->data)) {
      name = tp->name.name;
      span = tp->name.span;
      if (tp->constraint) {
        constraint = constraint_from_name(tp->constraint->name);
        if (constraint == TypeConstraint::None)
          bound = resolve_constraint_bound(*tp->constraint);
      }
    } else if (auto *ident = std::get_if<IdentifierNode>(&tp_node->data)) {
      // Legacy / instantiation-position shape; no constraint slot.
      name = ident->name;
      span = ident->span;
    } else {
      continue; // shape unrecognised; analyzer cannot bind it
    }

    uint32_t id = fresh_type_param_id();
    TypeParam tp{id, std::string(name), constraint};
    auto tp_type = make_type_param(
        id, tp.name, bound ? std::optional<TypePtr>(bound) : std::nullopt);
    if (tp_type) {
      auto &info = std::get<TypeParamInfo>(tp_type->detail);
      info.param.constraint = constraint;
    }

    declare(Symbol::type_param(tp.name, tp_type, span));
    current_scope->type_bindings[id] = tp_type;

    params.push_back(std::move(tp));
  }
  return params;
}

std::unordered_map<uint32_t, TypePtr> Analyzer::current_type_bindings() const {
  return current_scope->all_type_bindings();
}

// ===========================================================================
// Recording results
// ===========================================================================

void Analyzer::record_type(const Node &node, TypePtr type) {
  if (current_instantiation_) {
    current_instantiation_->node_types[&node] = std::move(type);
  } else {
    node_types[&node] = std::move(type);
  }
}

void Analyzer::record_symbol(const Node &node, const Symbol &sym) {
  // A package name reaches source only to be used — there is no write form to
  // hold apart, as there is for a variable — so binding one is using it.
  if (sym.kind == SymbolKind::Module)
    current_scope->mark_read(sym.name);

  if (current_instantiation_) {
    current_instantiation_->node_symbols[&node] = sym;
  } else {
    node_symbols[&node] = sym;
  }
}

// ===========================================================================
// Error reporting
// ===========================================================================

void Analyzer::error(Span span, const std::string &message) {
  if (silenced_)
    return;

  Position pos = fileset.position_at(span.start);

  // Append an "...instantiated from" frame for every active generic
  // instantiation so multi-level generic errors render a C++-template-style
  // backtrace rather than a single opaque in-body location.
  std::string full = message;
  for (auto it = instantiation_stack_.rbegin();
       it != instantiation_stack_.rend(); ++it) {
    const Node *call_node = *it;
    if (!call_node)
      continue;
    full += std::format("\n  ...instantiated from {}",
                        fileset.position_at(call_node->span.start));
  }

  errors.report_error(pos, full);
}

TypePtr Analyzer::poison(Span span, std::string reason) {
  // A silenced pass is explicitly one where failure is not a program error.
  if (!silenced_)
    deferred_bugs_.push_back({span, std::move(reason)});
  return builtins.invalid_type;
}

void Analyzer::report_deferred_bugs() {
  if (deferred_bugs_.empty() || !errors.errors.empty())
    return;

  auto &bug = deferred_bugs_.front();
  Position pos = fileset.position_at(bug.span.start);
  internal_error(std::format(
      "{}: analysis gave up here but reported nothing to the user: {}", pos,
      bug.reason));
}

void Analyzer::type_error(Span span, const TypePtr &expected,
                          const TypePtr &actual, const std::string &context) {
  std::string msg;
  if (context.empty()) {
    msg = std::format("type mismatch: expected {}, got {}",
                      type_to_string(expected), type_to_string(actual));
  } else {
    msg = std::format("{}: expected {}, got {}", context,
                      type_to_string(expected), type_to_string(actual));
  }
  error(span, msg);
}

void Analyzer::undefined_error(Span span, const std::string &name) {
  error(span, std::format("undefined name '{}'", name));
}

void Analyzer::redeclaration_error(Span span, const std::string &name) {
  error(span, std::format("'{}' already declared in this scope", name));
}

void Analyzer::shadowing_error(Span span, const std::string &name) {
  error(span, std::format("'{}' shadows a variable from an outer scope", name));
}

bool Analyzer::has_error_containing(const std::string &substr) const {
  for (auto &err : errors.errors) {
    if (err.message.find(substr) != std::string::npos)
      return true;
  }
  return false;
}

// ===========================================================================
// Validation helpers
// ===========================================================================

void Analyzer::expect_assignable(Span span, const TypePtr &target_type,
                                 const TypePtr &value_type,
                                 const std::string &context) {
  if (is_invalid_type(target_type) || is_invalid_type(value_type))
    return;
  if (is_assignable_to(value_type, target_type))
    return;
  // The bare is_assignable_to check in types.cpp can't see stdlib method
  // tables for primitive receivers (Int.Hash(), String.Equals(...)).  When
  // the target is an interface, fall back to the full structural check.
  if (target_type && target_type->kind == TypeKind::Interface &&
      satisfies_interface(value_type, target_type)) {
    return;
  }
  type_error(span, target_type, value_type, context);
}

void Analyzer::expect_type(Span span, const TypePtr &type, TypeKind expected,
                           const std::string &context) {
  if (is_invalid_type(type))
    return;
  if (type->kind != expected) {
    error(span, std::format("{}: expected {}, got {}", context,
                            type_to_string(
                                std::make_shared<Type>(expected, VoidType{})),
                            type_to_string(type)));
  }
}

void Analyzer::expect_bool(Span span, const TypePtr &type,
                           const std::string &context) {
  expect_assignable(span, builtins.bool_type, type, context);
}

// ===========================================================================
// Phase 1 — Declaration collection (top-level names)
// ===========================================================================

static std::optional<std::string> type_decl_name(const Node &node);

void Analyzer::visit_package(const PackageNode &pkg) {
  // Load stdlib type packages' receiver methods before analyzing user code.
  load_prelude();

  pending_type_decls_.clear();
  push_scope(ScopeKind::Module);

  // Save the package scope so import resolution can extract exports later.
  package_scope_ = current_scope;

  // Phase 1: collect all top-level names from ALL files (forward declarations).
  for (auto &src : pkg.sources) {
    auto &src_node = std::get<SourceNode>(src->data);
    for (auto &decl : src_node.declarations) {
      collect_declaration(*decl);
    }
  }

  // Phase 1.5: process imports from ALL files.
  for (auto &src : pkg.sources) {
    auto &src_node = std::get<SourceNode>(src->data);
    process_imports(src_node.declarations);
  }

  // Phase 2a: resolve type declarations (struct, enum, interface) from ALL
  // files first, so that function signatures can reference them.
  for (auto &src : pkg.sources) {
    auto &src_node = std::get<SourceNode>(src->data);
    for (auto &decl : src_node.declarations) {
      if (auto name = type_decl_name(*decl))
        ensure_type_resolved(*name);
    }
  }

  // Phase 2a.5: merge embedded interface method sets, now that every
  // interface's own methods are resolved.
  {
    std::vector<const InterfaceDeclNode *> ifaces;
    for (auto &src : pkg.sources) {
      auto &src_node = std::get<SourceNode>(src->data);
      for (auto &decl : src_node.declarations)
        if (auto *i = std::get_if<InterfaceDeclNode>(&decl->data))
          ifaces.push_back(i);
    }
    flatten_all_interfaces(ifaces);
  }

  // Phase 2b: resolve remaining declarations (functions, constants, etc.)
  // from ALL files.
  for (auto &src : pkg.sources) {
    auto &src_node = std::get<SourceNode>(src->data);
    for (auto &decl : src_node.declarations) {
      std::visit(overloaded{
                     [&](const FuncDeclNode &fn) { resolve_func_decl(fn); },
                     [&](const ConstDeclNode &c) { resolve_const_decl(c); },
                     [&](const StructDeclNode &) { /* done in phase 2a */ },
                     [&](const TypeDeclNode &) { /* done in phase 2a */ },
                     [&](const EnumDeclNode &) { /* done in phase 2a */ },
                     [&](const ErrorDeclNode &) { /* done in phase 2a */ },
                     [&](const InterfaceDeclNode &) { /* done in phase 2a */ },
                     [&](const ImportDeclNode &) { /* processed in phase 1.5 */ },
                     [&](const auto &) { /* already reported in collect */ },
                 },
                 decl->data);
    }
  }

  // Phase 3: resolve names inside function/method bodies from ALL files.
  for (auto &src : pkg.sources) {
    auto &src_node = std::get<SourceNode>(src->data);
    for (auto &decl : src_node.declarations) {
      std::visit(
          overloaded{
              [&](const FuncDeclNode &fn) { resolve_func_decl_body(fn); },
              [&](const auto &) {},
          },
          decl->data);
    }
  }

  // Phase 4: type-check top-level declarations and function bodies from ALL
  // files.
  for (auto &src : pkg.sources) {
    auto &src_node = std::get<SourceNode>(src->data);
    for (auto &decl : src_node.declarations) {
      std::visit(
          overloaded{
              [&](const FuncDeclNode &fn) { check_func_decl_body(fn); },
              [&](const StructDeclNode &s) { check_struct_decl(s); },
              [&](const EnumDeclNode &e) { check_enum_decl(e); },
              [&](const TypeDeclNode &t) { check_type_decl(t); },
              [&](const ErrorDeclNode &e) { check_error_decl(e); },
              [&](const InterfaceDeclNode &i) { check_interface_decl(i); },
              [&](const ConstDeclNode &c) { check_const_decl(c); },
              [&](const auto &) {},
          },
          decl->data);
    }
  }

  pop_module_scope();
}

void Analyzer::visit_source(const SourceNode &src) {
  pending_type_decls_.clear();
  push_scope(ScopeKind::Module);
  package_scope_ = current_scope;

  // Pass 1: collect all top-level names (forward declarations).
  for (auto &decl : src.declarations) {
    collect_declaration(*decl);
  }

  // Pass 1.5: process imports.
  process_imports(src.declarations);

  // Pass 2: resolve declaration types (struct fields, signatures, etc.).
  for (auto &decl : src.declarations) {
    resolve_declaration(*decl);
  }

  // Pass 2.5: merge embedded interface method sets.
  {
    std::vector<const InterfaceDeclNode *> ifaces;
    for (auto &decl : src.declarations)
      if (auto *i = std::get_if<InterfaceDeclNode>(&decl->data))
        ifaces.push_back(i);
    flatten_all_interfaces(ifaces);
  }

  // Pass 3: resolve names inside function/method bodies.
  for (auto &decl : src.declarations) {
    std::visit(overloaded{
                   [&](const FuncDeclNode &fn) { resolve_func_decl_body(fn); },
                   [&](const auto &) {},
               },
               decl->data);
  }

  // Pass 4: type-check top-level declarations and function bodies.
  for (auto &decl : src.declarations) {
    std::visit(overloaded{
                   [&](const FuncDeclNode &fn) { check_func_decl_body(fn); },
                   [&](const StructDeclNode &s) { check_struct_decl(s); },
                   [&](const EnumDeclNode &e) { check_enum_decl(e); },
                   [&](const TypeDeclNode &t) { check_type_decl(t); },
                   [&](const ErrorDeclNode &e) { check_error_decl(e); },
                   [&](const InterfaceDeclNode &i) { check_interface_decl(i); },
                   [&](const ConstDeclNode &c) { check_const_decl(c); },
                   [&](const auto &) {},
               },
               decl->data);
  }

  pop_module_scope();
}

void Analyzer::collect_declaration(const Node &node) {
  std::visit(overloaded{
                 [&](const FuncDeclNode &fn) {
                   // Receiver methods and type methods (`fn Type.Fn()`) are
                   // bound to a type; they're not callable as bare free
                   // functions and must not shadow types (e.g. Bool.String()
                   // must not shadow the String type).
                   if (!fn.receiver && !fn.type_name) {
                     auto sym = Symbol::function(std::string(fn.name.name),
                                                 nullptr, fn.name.span,
                                                 fn.is_public);
                     sym.is_extern = fn.is_extern;
                     declare(std::move(sym));
                   }
                 },
                 [&](const StructDeclNode &s) {
                   declare(Symbol::type_sym(std::string(s.name.name), nullptr,
                                            s.name.span, s.is_public));
                   pending_type_decls_[std::string(s.name.name)] = &node;
                 },
                 [&](const EnumDeclNode &e) {
                   declare(Symbol::type_sym(std::string(e.name.name), nullptr,
                                            e.name.span, e.is_public));
                   pending_type_decls_[std::string(e.name.name)] = &node;
                 },
                 [&](const ErrorDeclNode &e) {
                   declare(Symbol::type_sym(std::string(e.name.name), nullptr,
                                            e.name.span, e.is_public));
                   pending_type_decls_[std::string(e.name.name)] = &node;
                 },
                 [&](const InterfaceDeclNode &i) {
                   declare(Symbol::type_sym(std::string(i.name.name), nullptr,
                                            i.name.span, i.is_public));
                   pending_type_decls_[std::string(i.name.name)] = &node;
                 },
                 [&](const ConstDeclNode &c) {
                   declare(Symbol::constant(std::string(c.name.name), nullptr,
                                            c.name.span, c.is_public));
                 },
                 [&](const TypeDeclNode &t) {
                   declare(Symbol::type_sym(std::string(t.name.name), nullptr,
                                            t.name.span, t.is_public));
                   pending_type_decls_[std::string(t.name.name)] = &node;
                 },
                 [&](const ImportDeclNode &imp) {
                   // Derive the local name from the last path segment.
                   std::string path(imp.path);
                   auto last_slash = path.rfind('/');
                   std::string name = (last_slash != std::string::npos)
                                          ? path.substr(last_slash + 1)
                                          : path;
                   // Forward-declare the module symbol (type filled in during
                   // import processing).
                   declare(Symbol::module_sym(name, nullptr, imp.span));
                 },
                 [&](const auto &) {
                   error(node.span, "unexpected node at top level");
                 },
             },
             node.data);
}

// ===========================================================================
// Phase 1.5 — Import processing
// ===========================================================================

void Analyzer::process_imports(const std::vector<NodePtr> &declarations) {
  for (auto &decl : declarations) {
    std::visit(
        overloaded{
            [&](const ImportDeclNode &imp) {
              std::string path(imp.path);

              // Check for duplicate imports.
              if (imported_paths_.count(path)) {
                error(imp.span, std::format("duplicate import of '{}'", path));
                return;
              }
              imported_paths_.insert(path);

              // Derive the local name from the last path segment.
              auto last_slash = path.rfind('/');
              std::string name = (last_slash != std::string::npos)
                                     ? path.substr(last_slash + 1)
                                     : path;

              // Resolve the import to a module type.
              auto module_type = resolve_import(path, imp.span);

              // Update the forward-declared module symbol with the resolved
              // type.
              auto sym_it = current_scope->symbols.find(name);
              if (sym_it != current_scope->symbols.end()) {
                sym_it->second.type = module_type;
              }
            },
            [&](const ConstDeclNode &c) {
              // Handle `const Name = import "path"` — import expression
              // bound to a named constant.
              if (!c.value)
                return;
              auto *import_expr = std::get_if<ImportExprNode>(&c.value->data);
              if (!import_expr)
                return;

              std::string path(import_expr->path);

              // Check for duplicate imports.
              if (imported_paths_.count(path)) {
                error(c.value->span,
                      std::format("duplicate import of '{}'", path));
                return;
              }
              imported_paths_.insert(path);

              // Resolve the import.
              auto module_type = resolve_import(path, c.value->span);

              // Update the constant symbol to be a module symbol.
              auto sym_it =
                  current_scope->symbols.find(std::string(c.name.name));
              if (sym_it != current_scope->symbols.end()) {
                sym_it->second.type = module_type;
                sym_it->second.kind = SymbolKind::Module;
              }
            },
            [&](const auto &) { /* not an import */ },
        },
        decl->data);
  }
}

std::optional<TypePtr>
Analyzer::resolve_import_cached(const std::string &import_path, Span span) {
  if (!package_resolver)
    return std::nullopt;

  auto mock_it = package_resolver->mock_packages.find(import_path);
  if (mock_it != package_resolver->mock_packages.end())
    return mock_it->second;

  auto cache_it = package_resolver->cache.find(import_path);
  if (cache_it != package_resolver->cache.end())
    return cache_it->second;

  if (package_resolver->in_progress.count(import_path)) {
    error(span, std::format("circular import detected: '{}'", import_path));
    return builtins.invalid_type;
  }

  return std::nullopt;
}

void Analyzer::merge_sgi_receiver_methods(const SgiFile &sgi) {
  auto get_canonical = [this](const std::string &tn) -> const Type * {
    if (tn == "Int")    return builtins.int_type.get();
    if (tn == "Int8")   return builtins.int8_type.get();
    if (tn == "Int16")  return builtins.int16_type.get();
    if (tn == "Int32")  return builtins.int32_type.get();
    if (tn == "Int64")  return builtins.int64_type.get();
    if (tn == "Uint8")  return builtins.uint8_type.get();
    if (tn == "Uint16") return builtins.uint16_type.get();
    if (tn == "Uint32") return builtins.uint32_type.get();
    if (tn == "Uint64") return builtins.uint64_type.get();
    if (tn == "Float")  return builtins.float_type.get();
    if (tn == "Bool")   return builtins.bool_type.get();
    if (tn == "String") return builtins.string_type.get();
    return nullptr;
  };
  auto get_recv_kind = [](const std::string &tn) -> std::optional<TypeKind> {
    if (tn == "Array") return TypeKind::Array;
    if (tn == "Map")   return TypeKind::Map;
    return std::nullopt;
  };
  auto push_unique = [](std::vector<MethodInfo> &vec,
                        const MethodInfo &method) {
    for (auto &e : vec)
      if (e.name == method.name)
        return;
    vec.push_back(method);
  };

  for (auto &rm : sgi.receiver_methods) {
    if (const Type *canonical = get_canonical(rm.type_name)) {
      for (auto &method : rm.methods)
        push_unique(type_methods_[canonical], method);
      continue;
    }
    if (auto kind_opt = get_recv_kind(rm.type_name)) {
      auto kind = *kind_opt;
      for (auto &method : rm.methods) {
        auto sig = normalize_generic_receiver_sig(method.signature, kind);
        push_unique(kind_methods_[kind],
                    {method.name, sig, method.is_public});
      }
    }
  }
}

std::optional<TypePtr>
Analyzer::load_import_from_sgi(const std::string &import_path, Span span) {
  if (!package_resolver)
    return std::nullopt;

  std::string sgi_path = package_resolver->find_sgi_file(import_path);
  if (sgi_path.empty())
    return std::nullopt;

  std::string parse_err;
  auto sgi = load_sgi(sgi_path, &parse_err);
  if (!sgi) {
    error(span, std::format("failed to load interface for '{}' from '{}': {}",
                            import_path, sgi_path,
                            parse_err.empty() ? "unknown error" : parse_err));
    return std::nullopt;
  }

  auto module_type = sgi_to_module_type(*sgi, import_path);
  package_resolver->cache[import_path] = module_type;
  package_resolver->sgi_resolved_dirs[import_path] =
      fs::path(sgi_path).parent_path().string();
  if (!sgi->source_dir.empty()) {
    auto last_slash = import_path.rfind('/');
    std::string pkg_short = (last_slash != std::string::npos)
                                ? import_path.substr(last_slash + 1)
                                : import_path;
    package_resolver->source_dirs[pkg_short] = sgi->source_dir;
    package_resolver->source_dirs[import_path] = sgi->source_dir;
  }
  merge_sgi_receiver_methods(*sgi);
  return module_type;
}

void Analyzer::merge_sub_analyzer_receiver_methods(const Analyzer &sub) {
  auto map_scalar_to_canonical =
      [this](const Type *sub_type_ptr) -> const Type * {
    switch (sub_type_ptr->kind) {
    case TypeKind::Int: {
      auto &ii = std::get<IntType>(sub_type_ptr->detail);
      if (ii.bits == 0)
        return builtins.int_type.get();
      if (ii.is_signed) {
        switch (ii.bits) {
        case 8:  return builtins.int8_type.get();
        case 16: return builtins.int16_type.get();
        case 32: return builtins.int32_type.get();
        case 64: return builtins.int64_type.get();
        }
      } else {
        switch (ii.bits) {
        case 8:  return builtins.uint8_type.get();
        case 16: return builtins.uint16_type.get();
        case 32: return builtins.uint32_type.get();
        case 64: return builtins.uint64_type.get();
        }
      }
      return nullptr;
    }
    case TypeKind::Float:  return builtins.float_type.get();
    case TypeKind::Bool:   return builtins.bool_type.get();
    case TypeKind::String: return builtins.string_type.get();
    default: return nullptr;
    }
  };
  auto push_unique = [](std::vector<MethodInfo> &vec,
                        const MethodInfo &method) {
    for (auto &e : vec)
      if (e.name == method.name)
        return;
    vec.push_back(method);
  };

  for (auto &[sub_type_ptr, methods] : sub.type_methods_) {
    const Type *our_type = map_scalar_to_canonical(sub_type_ptr);
    if (!our_type)
      continue;
    for (auto &method : methods)
      push_unique(type_methods_[our_type], method);
  }

  for (auto &[kind, methods] : sub.kind_methods_)
    for (auto &method : methods)
      push_unique(kind_methods_[kind], method);
}

std::vector<ModuleExport>
Analyzer::extract_module_exports(const Analyzer &sub_analyzer) {
  std::vector<ModuleExport> exports;
  if (!sub_analyzer.package_scope_)
    return exports;
  for (auto &[sym_name, sym] : sub_analyzer.package_scope_->symbols)
    if (sym.is_public && !sym.is_builtin && sym.type)
      exports.push_back({sym_name, sym.type});
  return exports;
}

TypePtr
Analyzer::compile_import_from_source(const std::string &import_path, Span span) {
  if (!package_resolver) {
    error(span, std::format("cannot resolve import '{}': no package resolver",
                            import_path));
    return builtins.invalid_type;
  }

  std::string pkg_dir;
  if (import_path.starts_with("./") || import_path.starts_with("../")) {
    if (!current_package_dir.empty()) {
      pkg_dir = current_package_dir + "/" + import_path;
      if (std::filesystem::exists(pkg_dir))
        pkg_dir = std::filesystem::canonical(pkg_dir).string();
      else
        pkg_dir.clear();
    }
  } else {
    pkg_dir = package_resolver->find_package_dir(import_path);
  }

  if (pkg_dir.empty()) {
    error(span, std::format("cannot find package '{}'", import_path));
    return builtins.invalid_type;
  }

  auto source_files = package_resolver->list_source_files(pkg_dir);
  if (source_files.empty()) {
    error(span,
          std::format("package '{}' contains no source files", import_path));
    return builtins.invalid_type;
  }

  package_resolver->in_progress.insert(import_path);

  FileSet sub_fileset;
  for (auto &f : source_files) {
    auto file = File::from_path(f);
    if (file)
      sub_fileset.add_file(std::move(file));
  }

  if (sub_fileset.files.empty()) {
    error(span, std::format("failed to read source files for package '{}'",
                            import_path));
    package_resolver->in_progress.erase(import_path);
    return builtins.invalid_type;
  }

  Parser sub_parser(sub_fileset);
  auto sub_ast = sub_parser.parse();
  if (!sub_ast || !sub_parser.errors.errors.empty()) {
    error(span,
          std::format("parse errors in imported package '{}'", import_path));
    package_resolver->in_progress.erase(import_path);
    return builtins.invalid_type;
  }

  auto pkg_last_slash = import_path.rfind('/');
  std::string pkg_short = (pkg_last_slash != std::string::npos)
                              ? import_path.substr(pkg_last_slash + 1)
                              : import_path;

  Analyzer sub_analyzer(sub_fileset, package_resolver);
  sub_analyzer.current_package_dir = pkg_dir;
  sub_analyzer.current_package_name_override = pkg_short;
  sub_analyzer.analyze(*sub_ast);

  if (!sub_analyzer.errors.errors.empty()) {
    error(span, std::format("errors in imported package '{}'", import_path));
    for (auto &e : sub_analyzer.errors.errors)
      errors.errors.push_back(e);
    package_resolver->in_progress.erase(import_path);
    return builtins.invalid_type;
  }

  merge_sub_analyzer_receiver_methods(sub_analyzer);

  const std::string &pkg_name = pkg_short;
  auto exports = extract_module_exports(sub_analyzer);
  auto module_type =
      make_module_type(pkg_name, import_path, std::move(exports));

  package_resolver->cache[import_path] = module_type;
  package_resolver->in_progress.erase(import_path);

  // Record the source directory under both the short package name and the
  // full import path so codegen can find generic method bodies later.
  std::error_code abs_ec;
  std::string abs_pkg_dir =
      fs::absolute(pkg_dir, abs_ec).lexically_normal().string();
  if (!abs_ec) {
    package_resolver->source_dirs[pkg_name] = abs_pkg_dir;
    package_resolver->source_dirs[import_path] = abs_pkg_dir;
  }
  return module_type;
}

TypePtr Analyzer::resolve_import(const std::string &import_path, Span span) {
  if (auto cached = resolve_import_cached(import_path, span))
    return *cached;
  size_t err_count_before = errors.errors.size();
  if (auto from_sgi = load_import_from_sgi(import_path, span))
    return *from_sgi;
  // If load_import_from_sgi reported a parse error, don't fall through to
  // source compilation — surface the diagnostic instead of silently masking
  // a malformed .sgi with a fresh source build.
  if (errors.errors.size() > err_count_before)
    return builtins.invalid_type;
  return compile_import_from_source(import_path, span);
}

// ===========================================================================
// Cross-package generic method body loading (D8)
// ===========================================================================

Analyzer *Analyzer::ensure_source_loaded(const std::string &origin) {
  if (origin.empty()) return nullptr;
  auto it = loaded_source_packages_.find(origin);
  if (it != loaded_source_packages_.end())
    return it->second.sub_analyzer.get();

  if (!package_resolver) return nullptr;
  std::string source_dir;
  auto dir_it = package_resolver->source_dirs.find(origin);
  if (dir_it != package_resolver->source_dirs.end())
    source_dir = dir_it->second;
  else
    source_dir = package_resolver->find_package_dir(origin);
  if (source_dir.empty() || !fs::is_directory(source_dir))
    return nullptr;

  auto source_files = package_resolver->list_source_files(source_dir);
  if (source_files.empty())
    return nullptr;

  auto loaded_fileset = std::make_unique<FileSet>();
  for (auto &f : source_files) {
    auto file = File::from_path(f);
    if (file)
      loaded_fileset->add_file(std::move(file));
  }
  if (loaded_fileset->files.empty())
    return nullptr;

  Parser sub_parser(*loaded_fileset);
  auto sub_ast = sub_parser.parse();
  if (!sub_ast || !sub_parser.errors.errors.empty())
    return nullptr;

  auto sub_analyzer =
      std::make_unique<Analyzer>(*loaded_fileset, package_resolver);
  sub_analyzer->current_package_dir = source_dir;
  // ensure_source_loaded is keyed by origin (package name), so it doubles
  // as the authoritative name for the loaded sub-analyzer.
  sub_analyzer->current_package_name_override = origin;
  // Inherit stdlib mode when loading a stdlib package's source — stdlib
  // packages use intrinsics and define receiver methods on intrinsic
  // types, both of which are gated to stdlib mode.
  static const std::unordered_set<std::string> stdlib_origins{
      "proto", "int", "float", "bool", "string", "array", "map",
      "unsafe", "sys", "os", "io"};
  if (stdlib_origins.count(origin))
    sub_analyzer->is_stdlib = true;
  sub_analyzer->analyze(*sub_ast);
  if (!sub_analyzer->errors.errors.empty())
    return nullptr;

  auto *sub_ptr = sub_analyzer.get();

  // Merge node-keyed tables. Keys are pointers into the sub-AST that we
  // hold alive in loaded_source_packages_, so they remain valid for the
  // lifetime of this analyzer.
  for (auto &[k, v] : sub_ptr->node_types) node_types.emplace(k, v);
  for (auto &[k, v] : sub_ptr->node_symbols) node_symbols.emplace(k, v);
  for (auto &[k, v] : sub_ptr->node_captures) node_captures.emplace(k, v);
  for (auto &[k, v] : sub_ptr->node_type_args) node_type_args.emplace(k, v);
  for (auto &[k, v] : sub_ptr->iterable_next_elem_type)
    iterable_next_elem_type.emplace(k, v);
  for (auto &[k, v] : sub_ptr->spawn_channel_elem_types)
    spawn_channel_elem_types.emplace(k, v);
  for (auto &[k, v] : sub_ptr->spawn_captures) spawn_captures.emplace(k, v);
  for (auto &[k, v] : sub_ptr->struct_operator_methods)
    struct_operator_methods.emplace(k, v);
  for (auto &[k, v] : sub_ptr->generic_templates_)
    generic_templates_.emplace(k, v);
  for (auto &[k, v] : sub_ptr->func_decl_by_type_)
    func_decl_by_type_.emplace(k, v);
  for (auto *fd : sub_ptr->kind_method_uses_typeparam_dispatch_)
    kind_method_uses_typeparam_dispatch_.insert(fd);
  for (auto &[kind, methods] : sub_ptr->kind_method_decls_) {
    auto &dst = kind_method_decls_[kind];
    for (auto &[name, kmd] : methods) {
      dst.emplace(name, kmd);
    }
  }

  loaded_source_packages_.emplace(
      origin,
      LoadedSourcePackage{std::move(loaded_fileset), std::move(sub_ast),
                          std::move(sub_analyzer)});
  return sub_ptr;
}

Analyzer::GenericMethodDecl
Analyzer::generic_method_decl(const std::string &struct_name,
                              const std::string &method_name,
                              const std::vector<TypePtr> &type_args) {
  GenericMethodDecl result;
  if (!package_scope_) return result;

  auto sym_it = package_scope_->symbols.find(struct_name);
  if (sym_it == package_scope_->symbols.end()) return result;
  auto struct_type = sym_it->second.type;
  if (!struct_type || struct_type->kind != TypeKind::Struct) return result;

  auto &sinfo = std::get<StructTypeInfo>(struct_type->detail);
  for (auto &m : sinfo.methods) {
    if (m.name != method_name || !m.signature) continue;
    auto fd_it = func_decl_by_type_.find(m.signature.get());
    if (fd_it == func_decl_by_type_.end()) return result;
    result.decl = fd_it->second;
    result.template_signature = m.signature;
    result.struct_type_params = sinfo.type_params;
    break;
  }
  if (!result.decl) return result;

  // Drive body type-checking under the caller's bindings so codegen can
  // read concrete TypePtrs out of the resulting BodyInstantiation. Bind by
  // the function's GenericTemplate IDs (which P5's receiver remap aligned
  // with the struct's type-param IDs).
  auto tpl_it = generic_templates_.find(result.decl);
  if (tpl_it != generic_templates_.end()) {
    auto &tpl_params = tpl_it->second.type_params;
    size_t n = std::min(tpl_params.size(), type_args.size());
    for (size_t i = 0; i < n; ++i)
      result.bindings[tpl_params[i].id] = type_args[i];
  } else {
    // Fall back to positional alignment with the struct's type params if
    // the template wasn't recorded (e.g. non-generic receiver method).
    size_t n =
        std::min(result.struct_type_params.size(), type_args.size());
    for (size_t i = 0; i < n; ++i)
      result.bindings[result.struct_type_params[i].id] = type_args[i];
  }
  if (!result.bindings.empty() && result.decl->body) {
    result.instantiation =
        instantiate_generic_body(*result.decl, result.bindings,
                                 *result.decl->body);
  }
  return result;
}

Analyzer::GenericMethodDecl
Analyzer::load_imported_method_decl(const std::string &origin,
                                     const std::string &struct_name,
                                     const std::string &method_name,
                                     const std::vector<TypePtr> &type_args) {
  auto *sub = ensure_source_loaded(origin);
  if (!sub) return {};
  return sub->generic_method_decl(struct_name, method_name, type_args);
}

// ===========================================================================
// Phase 2 — Type resolution (annotations → TypePtrs)
// ===========================================================================

TypePtr Analyzer::resolve_type(const Node &node) {
  return std::visit(
      overloaded{
          [&](const IdentifierNode &n) -> TypePtr {
            return resolve_identifier_type(n);
          },
          [&](const ArrayTypeNode &n) -> TypePtr {
            return resolve_array_type(n);
          },
          [&](const MapTypeNode &n) -> TypePtr { return resolve_map_type(n); },
          [&](const FuncTypeNode &n) -> TypePtr {
            return resolve_func_type(n);
          },
          [&](const UnionTypeNode &n) -> TypePtr {
            return resolve_union_type(n);
          },
          [&](const GenericTypeAppNode &n) -> TypePtr {
            return resolve_generic_type_app(n);
          },
          [&](const SelectorNode &n) -> TypePtr {
            return resolve_selector_type(n);
          },
          [&](const auto &) -> TypePtr {
            error(node.span, "expected type expression");
            return builtins.invalid_type;
          },
      },
      node.data);
}

TypePtr Analyzer::resolve_identifier_type(const IdentifierNode &node) {
  std::string name(node.name);
  auto sym = lookup(name);
  if (!sym) {
    undefined_error(node.span, name);
    return builtins.invalid_type;
  }
  if (sym->kind != SymbolKind::Type && sym->kind != SymbolKind::TypeParam) {
    error(node.span, std::format("'{}' is not a type", name));
    return builtins.invalid_type;
  }
  if (!sym->type) {
    ensure_type_resolved(name);
    sym = lookup(name);
  }
  return sym && sym->type ? sym->type : builtins.invalid_type;
}

TypePtr Analyzer::resolve_selector_type(const SelectorNode &node) {
  auto *obj_ident = std::get_if<IdentifierNode>(&node.object->data);
  if (!obj_ident) {
    error(node.span, "expected package name in qualified type");
    return builtins.invalid_type;
  }
  auto mod_sym = lookup(std::string(obj_ident->name));
  if (!mod_sym || !mod_sym->type ||
      mod_sym->type->kind != TypeKind::Module) {
    error(obj_ident->span,
          std::format("'{}' is not a package", obj_ident->name));
    return builtins.invalid_type;
  }
  record_symbol(*node.object, *mod_sym);
  auto &mod = std::get<ModuleTypeInfo>(mod_sym->type->detail);
  std::string type_name(node.field.name);
  for (auto &exp : mod.exports) {
    if (exp.name == type_name && exp.type)
      return exp.type;
  }
  error(node.field.span,
        std::format("package '{}' has no exported type '{}'",
                    mod.name, type_name));
  return builtins.invalid_type;
}

TypePtr Analyzer::resolve_array_type(const ArrayTypeNode &node) {
  auto elem = resolve_type(*node.element_type);
  return make_array_type(std::move(elem));
}

TypePtr Analyzer::resolve_map_type(const MapTypeNode &node) {
  auto key = resolve_type(*node.key_type);
  auto val = resolve_type(*node.value_type);
  check_satisfies_protocol(key, ProtocolKind::Hashable, node.key_type->span,
                           "map key");
  return make_map_type(std::move(key), std::move(val));
}

TypePtr Analyzer::resolve_func_type(const FuncTypeNode &node) {
  std::vector<TypePtr> params;
  for (auto &p : node.params)
    params.push_back(resolve_type(*p));
  TypePtr ret = node.return_type ? resolve_type(*node.return_type) : nullptr;
  return make_func_type(std::move(params), std::move(ret));
}

TypePtr Analyzer::resolve_union_type(const UnionTypeNode &node) {
  std::vector<TypePtr> alts;
  std::vector<TypePtr> seen; // flattened members, for duplicate detection
  for (auto &t : node.types) {
    auto rt = resolve_type(*t);
    alts.push_back(rt);

    // Rule 1 — concrete types only. A TypeParam is allowed (concrete after
    // instantiation); an interface (by any alias) is not.
    if (auto u = unwrap_alias(rt); u && u->kind == TypeKind::Interface) {
      error(t->span,
            std::format("a type union may only contain concrete types; "
                        "'{}' is an interface",
                        type_to_string(rt)));
      continue;
    }

    // Rule 2 — a union is a set of values, and `void` is the absence of one.
    // What reaches for this slot is a value carrying no information, which is
    // a different thing and has its own name.
    if (auto u = unwrap_alias(rt); u && u->kind == TypeKind::Void) {
      error(t->span,
            "a type union may only contain values; 'void' is the absence of a "
            "value — use 'Null' for a value that carries nothing");
      continue;
    }

    // Rule 3 — the composed set must be unique. A union alternative is spliced,
    // so `T | (T | U)` and `T | T` both report the repeat.
    for (auto &m : flatten_union_alternatives({rt})) {
      bool dup = false;
      for (auto &s : seen)
        if (types_equal(s, m)) { dup = true; break; }
      if (dup)
        error(t->span, std::format("duplicate type '{}' in union",
                                   type_to_string(m)));
      else
        seen.push_back(m);
    }
  }
  return make_union_type(std::move(alts));
}

// Whether the arguments are the struct's own parameters, in order.
static bool names_own_params(const StructTypeInfo &info,
                             const std::vector<TypePtr> &args) {
  if (args.size() != info.type_params.size())
    return false;
  for (size_t i = 0; i < args.size(); ++i) {
    if (!args[i] || args[i]->kind != TypeKind::TypeParam)
      return false;
    if (std::get<TypeParamInfo>(args[i]->detail).param.id !=
        info.type_params[i].id)
      return false;
  }
  return true;
}

TypePtr
Analyzer::resolve_generic_type_app(const GenericTypeAppNode &node) {
  auto base = resolve_type(*node.base_type);
  if (is_invalid_type(base))
    return builtins.invalid_type;

  // Resolve concrete type arguments.
  std::vector<TypePtr> args;
  for (auto &ta : node.type_args)
    args.push_back(resolve_type(*ta));

  // The base must be a generic struct (Task, etc.).
  if (base->kind != TypeKind::Struct) {
    error(node.span,
          std::format("type {} is not generic", type_to_string(base)));
    return builtins.invalid_type;
  }

  auto &info = std::get<StructTypeInfo>(base->detail);
  if (info.type_params.empty()) {
    error(node.span,
          std::format("type {} is not generic", type_to_string(base)));
    return builtins.invalid_type;
  }

  if (args.size() != info.type_params.size()) {
    error(node.span,
          std::format("expected {} type argument(s), got {}",
                      info.type_params.size(), args.size()));
    return builtins.invalid_type;
  }

  // Inside its own body a struct names itself: `Node<T>` there is this very
  // declaration, whose fields are still being filled. Instantiating now would
  // snapshot an empty field list.
  if (resolving_structs_.count(info.name) && names_own_params(info, args))
    return base;

  // Build bindings and instantiate.
  std::unordered_map<uint32_t, TypePtr> bindings;
  for (size_t i = 0; i < info.type_params.size(); ++i)
    bindings[info.type_params[i].id] = args[i];

  std::vector<FieldInfo> new_fields;
  for (auto &f : info.fields)
    new_fields.push_back({f.name, substitute(f.type, bindings), f.is_public});
  std::vector<MethodInfo> new_methods;
  for (auto &m : info.methods)
    new_methods.push_back(
        {m.name, substitute(m.signature, bindings), m.is_public,
         m.origin_package});

  auto result =
      make_struct_type(info.name, std::move(new_fields), std::move(new_methods),
                       {}, info.origin_package);
  auto &ri = std::get<StructTypeInfo>(result->detail);
  ri.type_params = info.type_params;
  ri.type_args = std::move(args);
  ri.embeds = info.embeds;
  return result;
}

// ===========================================================================
// Phase 2b — Resolve top-level declaration types
// ===========================================================================

// `void` is the absence of a value. It can be returned and it can be a union
// alternative — that is what `T | void` means — but a value of it does not
// exist, so nothing can hold one.
static bool holds_void(const TypePtr &t) {
  auto u = unwrap_alias(t);
  if (!u)
    return false;
  switch (u->kind) {
  case TypeKind::Void:
    return true;
  case TypeKind::Array:
    return holds_void(std::get<ArrayTypeInfo>(u->detail).element);
  case TypeKind::Map: {
    auto &info = std::get<MapTypeInfo>(u->detail);
    return holds_void(info.key) || holds_void(info.value);
  }
  default:
    return false;
  }
}

bool Analyzer::reject_void_value(Span span, const TypePtr &type,
                                 std::string_view what) {
  if (!holds_void(type))
    return false;
  error(span, std::format("{} cannot be typed '{}': void is the absence of a "
                          "value, so there is nothing to hold",
                          what, type_to_string(type)));
  return true;
}

bool Analyzer::reject_void_bindings(
    const std::unordered_map<uint32_t, TypePtr> &bindings, Span span) {
  // `bindings` is unordered, so walk the ids in order or the diagnostics come
  // out differently from one run to the next.
  std::vector<uint32_t> ids;
  for (auto &[id, concrete] : bindings)
    ids.push_back(id);
  std::sort(ids.begin(), ids.end());

  bool rejected = false;
  for (uint32_t id : ids)
    rejected |=
        reject_void_value(span, bindings.at(id), "a type argument");
  return rejected;
}

TypePtr Analyzer::resolve_signature(const SignatureNode &sig) {
  std::vector<TypePtr> params;
  for (auto &p : sig.params) {
    auto pt = resolve_type(*p.type);
    reject_void_value(p.type->span, pt, "a parameter");
    // Each name in the identifier list maps to one parameter of that type.
    // An empty identifier list means an unnamed parameter (Go-style interface
    // method) — push the type once.
    if (p.names.identifiers.empty()) {
      params.push_back(p.is_variadic ? make_array_type(pt) : pt);
      continue;
    }
    for (size_t i = 0; i < p.names.identifiers.size(); ++i) {
      if (p.is_variadic && i == p.names.identifiers.size() - 1) {
        // Variadic wraps the type in an array.
        params.push_back(make_array_type(pt));
      } else {
        params.push_back(pt);
      }
    }
  }
  TypePtr ret = sig.return_type ? resolve_type(*sig.return_type) : nullptr;
  // Record the resolved return type so codegen can recover it without
  // re-resolving the AST against a scope where package-local names (a user
  // error `E` in `int | E`) aren't reachable.
  if (sig.return_type && ret)
    record_type(*sig.return_type, ret);
  return make_func_type(std::move(params), std::move(ret));
}

void Analyzer::declare_parameters(const SignatureNode &sig) {
  for (auto &p : sig.params) {
    auto pt = resolve_type(*p.type);
    for (auto &ident : p.names.identifiers) {
      auto param_type = p.is_variadic ? make_array_type(pt) : pt;
      declare_local(Symbol::parameter(std::string(ident.name),
                                      std::move(param_type), ident.span));
    }
  }
}

static std::optional<std::string> type_decl_name(const Node &node) {
  using R = std::optional<std::string>;
  return std::visit(
      overloaded{
          [](const StructDeclNode &s) -> R { return std::string(s.name.name); },
          [](const EnumDeclNode &e) -> R { return std::string(e.name.name); },
          [](const ErrorDeclNode &e) -> R { return std::string(e.name.name); },
          [](const InterfaceDeclNode &i) -> R {
            return std::string(i.name.name);
          },
          [](const TypeDeclNode &t) -> R { return std::string(t.name.name); },
          [](const auto &) -> R { return std::nullopt; },
      },
      node.data);
}

void Analyzer::ensure_type_resolved(const std::string &name) {
  auto it = pending_type_decls_.find(name);
  if (it == pending_type_decls_.end())
    return;
  const Node *decl = it->second;
  // Drop the entry before resolving: a type that names itself, or a peer that
  // names it back, re-enters here. Each resolver publishes its type into the
  // symbol before descending into fields, so the cycle sees a real type.
  pending_type_decls_.erase(it);
  AtPackageScope at_package(*this);
  std::visit(overloaded{
                 [&](const StructDeclNode &s) { resolve_struct_decl(s); },
                 [&](const EnumDeclNode &e) { resolve_enum_decl(e); },
                 [&](const ErrorDeclNode &e) { resolve_error_decl(e); },
                 [&](const InterfaceDeclNode &i) { resolve_interface_decl(i); },
                 [&](const TypeDeclNode &t) { resolve_type_decl(t); },
                 [&](const auto &) {},
             },
             decl->data);
}

void Analyzer::resolve_declaration(const Node &node) {
  if (auto name = type_decl_name(node))
    return ensure_type_resolved(*name);
  std::visit(overloaded{
                 [&](const FuncDeclNode &fn) { resolve_func_decl(fn); },
                 [&](const ConstDeclNode &c) { resolve_const_decl(c); },
                 [&](const ImportDeclNode &) { /* processed in phase 1.5 */ },
                 [&](const auto &) { /* already reported in collect */ },
             },
             node.data);
}

void Analyzer::resolve_func_decl(const FuncDeclNode &fn) {
  // If the function is generic, push a temporary scope to hold type params
  // so the signature can reference them.  The scope must also be active
  // while resolving the receiver type, so we defer pop_scope() until after
  // the receiver binding below.
  bool has_generics = fn.generic.has_value();
  std::vector<TypeParam> generic_params;
  if (has_generics) {
    push_scope(ScopeKind::Block);
    generic_params = enter_generics(*fn.generic);
  }

  auto fn_type = resolve_signature(fn.signature);

  // Mark the function type as variadic if the last param is.
  if (!fn.signature.params.empty() && fn.signature.params.back().is_variadic) {
    auto &fi = std::get<FuncTypeInfo>(fn_type->detail);
    fi.is_variadic = true;
  }

  // If this is a receiver method, attach it to the receiver type.
  // NOTE: generics scope (if any) is still active here so that non-identifier
  // receiver types like [T] can resolve T as the function's type parameter.
  if (fn.receiver) {
    auto &recv_type_node = fn.receiver->type;
    // Helper: check D4 — cannot bind a receiver to a type from another package.
    auto check_recv_origin = [&](const std::string &origin,
                                 const std::string &type_name) -> bool {
      if (!origin.empty() && origin != current_package_name()) {
        error(fn.receiver->name.span,
              std::format("cannot bind receiver method to type '{}' from "
                          "another package",
                          type_name));
        return false;
      }
      return true;
    };

    if (auto *gapp =
            std::get_if<GenericTypeAppNode>(&recv_type_node->data)) {
      // Generic type application receiver: pub fn |T| (b |T| Box) Method(...)
      // Attach the method to the base struct type (ignoring the type args).
      if (auto *base_ident =
              std::get_if<IdentifierNode>(&gapp->base_type->data)) {
        auto recv_sym = lookup(std::string(base_ident->name));
        if (recv_sym && recv_sym->type &&
            recv_sym->type->kind == TypeKind::Struct) {
          auto &struct_info =
              std::get<StructTypeInfo>(recv_sym->type->detail);
          if (check_recv_origin(struct_info.origin_package,
                                struct_info.name)) {
            // Remap the function's TypeParam IDs to the struct's TypeParam IDs
            // so that instantiate_generic_struct can substitute them correctly.
            // e.g. fn |T#5| (b |T#5| Box) Get() T#5  →  Get() T#0 (struct's T)
            TypePtr stored_sig = fn_type;
            std::unordered_map<uint32_t, uint32_t> id_remap;
            if (!struct_info.type_params.empty() &&
                gapp->type_args.size() == struct_info.type_params.size()) {
              std::unordered_map<uint32_t, TypePtr> remap;
              for (size_t i = 0; i < gapp->type_args.size(); ++i) {
                auto arg_t = resolve_type(*gapp->type_args[i]);
                if (arg_t && arg_t->kind == TypeKind::TypeParam) {
                  auto &tp = std::get<TypeParamInfo>(arg_t->detail);
                  remap[tp.param.id] =
                      make_type_param(struct_info.type_params[i].id,
                                      struct_info.type_params[i].name);
                  id_remap[tp.param.id] = struct_info.type_params[i].id;
                }
              }
              if (!remap.empty())
                stored_sig = substitute(fn_type, remap);
            }
            // Apply the same remap to the function's generic_params so that
            // generic_templates_[&fn] uses struct-aligned IDs. This keeps
            // bindings derived from the struct (instantiate_generic_struct,
            // cross-package method specialisation) keyed compatibly with the
            // GenericTemplate, both for ordered-args lookup in
            // emit_specialisation and for substitute() over the stored sig.
            for (auto &gp : generic_params) {
              auto it = id_remap.find(gp.id);
              if (it != id_remap.end())
                gp.id = it->second;
            }
            // Also register the struct-aligned signature so that
            // load_imported_method_decl (which iterates the struct's methods)
            // can resolve back to this FuncDeclNode.
            if (stored_sig && stored_sig.get() != fn_type.get())
              func_decl_by_type_[stored_sig.get()] = &fn;
            struct_info.methods.push_back(
                {std::string(fn.name.name), stored_sig, fn.is_public,
                 current_package_name()});
          }
        }
      }
    } else if (auto *ident =
                   std::get_if<IdentifierNode>(&recv_type_node->data)) {
      auto recv_sym = lookup(std::string(ident->name));
      if (recv_sym && recv_sym->type) {
        if (recv_sym->type->kind == TypeKind::Struct) {
          auto &struct_info = std::get<StructTypeInfo>(recv_sym->type->detail);
          if (struct_info.is_error) {
            error(recv_type_node->span,
                  std::format("methods cannot be attached to error type '{}'",
                              struct_info.name));
          } else if (check_recv_origin(struct_info.origin_package,
                                       struct_info.name)) {
            struct_info.methods.push_back(
                {std::string(fn.name.name), fn_type, fn.is_public,
                 current_package_name()});
          }
        } else if (recv_sym->type->kind == TypeKind::Alias) {
          auto &alias_info = std::get<AliasTypeInfo>(recv_sym->type->detail);
          if (alias_info.structural) {
            error(recv_type_node->span,
                  std::format("cannot attach methods to structural alias '{}'; "
                              "declare it nominal (`type {} {}`) to add methods",
                              alias_info.name, alias_info.name,
                              type_to_string(alias_info.underlying)));
          } else {
            alias_info.methods.push_back(
                {std::string(fn.name.name), fn_type, fn.is_public,
                 current_package_name()});
          }
        } else if (recv_sym->type->kind == TypeKind::Enum) {
          // Enums can also have methods bound to them.
          // Store in the type_methods side table.
          type_methods_[recv_sym->type.get()].push_back(
              {std::string(fn.name.name), fn_type, fn.is_public});
        } else if (recv_sym->type->kind == TypeKind::Int ||
                   recv_sym->type->kind == TypeKind::Float ||
                   recv_sym->type->kind == TypeKind::Bool ||
                   recv_sym->type->kind == TypeKind::String) {
          // Receiver methods on intrinsic types — stdlib only.
          if (!is_stdlib) {
            error(fn.receiver->name.span,
                  "receiver methods on intrinsic types can only be "
                  "defined in stdlib packages");
          } else {
            type_methods_[recv_sym->type.get()].push_back(
                {std::string(fn.name.name), fn_type, fn.is_public});
          }
        }
      }
    } else if (std::get_if<SelectorNode>(&recv_type_node->data)) {
      // Qualified receiver: pub fn (self pkg.Type) Method(...)
      // D4: types from other packages always have a different origin — reject.
      error(fn.receiver->name.span,
            "cannot bind receiver method to type from another package");
    } else if (auto *arr_tn =
                   std::get_if<ArrayTypeNode>(&recv_type_node->data)) {
      // Generic array receiver: pub fn (self [T]) Method(...) ...
      // Stdlib only. Normalize the T type-param ID to the sentinel 9990 so
      // that check_selector's existing substitution logic handles it.
      if (!is_stdlib) {
        error(fn.receiver->name.span,
              "receiver methods on generic types can only be "
              "defined in stdlib packages");
      } else {
        auto elem_type = resolve_type(*arr_tn->element_type);
        TypePtr normalized = fn_type;
        if (elem_type && elem_type->kind == TypeKind::TypeParam) {
          auto &tp = std::get<TypeParamInfo>(elem_type->detail);
          std::unordered_map<uint32_t, TypePtr> subst;
          subst[tp.param.id] = make_type_param(9990, "T");
          normalized = substitute(fn_type, subst);
        }
        auto &vec = kind_methods_[TypeKind::Array];
        bool dup = false;
        for (auto &e : vec)
          if (e.name == std::string(fn.name.name)) { dup = true; break; }
        if (!dup) {
          vec.push_back({std::string(fn.name.name), normalized, fn.is_public});
          kind_method_decls_[TypeKind::Array][std::string(fn.name.name)] =
              KindMethodDecl{&fn, fn_type, generic_params};
        }
      }
    } else if (auto *map_tn =
                   std::get_if<MapTypeNode>(&recv_type_node->data)) {
      // Generic map receiver: pub fn (self {K:V}) Method(...) ...
      // Stdlib only. Normalize K→9991, V→9992.
      if (!is_stdlib) {
        error(fn.receiver->name.span,
              "receiver methods on generic types can only be "
              "defined in stdlib packages");
      } else {
        auto key_type = resolve_type(*map_tn->key_type);
        auto val_type = resolve_type(*map_tn->value_type);
        TypePtr normalized = fn_type;
        std::unordered_map<uint32_t, TypePtr> subst;
        if (key_type && key_type->kind == TypeKind::TypeParam)
          subst[std::get<TypeParamInfo>(key_type->detail).param.id] =
              make_type_param(9991, "K");
        if (val_type && val_type->kind == TypeKind::TypeParam)
          subst[std::get<TypeParamInfo>(val_type->detail).param.id] =
              make_type_param(9992, "V");
        if (!subst.empty())
          normalized = substitute(fn_type, subst);
        auto &vec = kind_methods_[TypeKind::Map];
        bool dup = false;
        for (auto &e : vec)
          if (e.name == std::string(fn.name.name)) { dup = true; break; }
        if (!dup) {
          vec.push_back({std::string(fn.name.name), normalized, fn.is_public});
          kind_method_decls_[TypeKind::Map][std::string(fn.name.name)] =
              KindMethodDecl{&fn, fn_type, generic_params};
        }
      }
    }
  }

  if (fn.type_name && !fn.receiver)
    attach_type_method(fn, fn_type);

  if (has_generics)
    pop_scope();

  // Update the function symbol.
  auto sym_it = current_scope->symbols.find(std::string(fn.name.name));
  if (sym_it != current_scope->symbols.end()) {
    sym_it->second.type = fn_type;
  }

  // Record the reverse FuncDecl lookup so that check_call_expr can get
  // from a callee's type back to its AST for body instantiation.
  // For receiver methods on concrete types (e.g. generic methods on a
  // user struct), we also register here so the monomorphiser can find them.
  bool is_generic_on_concrete_recv =
      has_generics && fn.receiver &&
      fn_type && fn_type->kind == TypeKind::Func;
  // Generic receiver on Array/Map (kind_methods_) is also registered so
  // codegen can drive emit_specialisation per concrete K when the body
  // dispatches through a named protocol on a TypeParam value.
  bool is_generic_on_kind_recv = false;
  if (has_generics && fn.receiver) {
    auto &rt = fn.receiver->type->data;
    is_generic_on_kind_recv = std::get_if<ArrayTypeNode>(&rt) ||
                              std::get_if<MapTypeNode>(&rt);
  }
  if (fn_type && (!fn.receiver || is_generic_on_concrete_recv ||
                   is_generic_on_kind_recv)) {
    func_decl_by_type_[fn_type.get()] = &fn;
  }

  // Stash the generic template for lazy body-analysis at instantiation
  // time.  Generic receiver methods on concrete types are included so
  // their bodies are checked per-instantiation (same as free generics).
  if (has_generics && (!fn.receiver || is_generic_on_concrete_recv ||
                        is_generic_on_kind_recv)) {
    generic_templates_[&fn] =
        GenericTemplate{&fn, current_scope, std::move(generic_params),
                        is_stdlib};
  }
}

void Analyzer::attach_type_method(const FuncDeclNode &fn,
                                  const TypePtr &fn_type) {
  auto type_sym = lookup(std::string(fn.type_name->name));
  if (!type_sym || !type_sym->type || type_sym->kind != SymbolKind::Type) {
    error(fn.type_name->span,
          std::format("unknown type '{}' for type method", fn.type_name->name));
    return;
  }
  if (type_sym->type->kind != TypeKind::Struct) {
    error(fn.type_name->span,
          std::format("type methods are only supported on structs; '{}' is not "
                      "a struct",
                      fn.type_name->name));
    return;
  }
  auto &sinfo = std::get<StructTypeInfo>(type_sym->type->detail);
  if (sinfo.is_error) {
    error(fn.type_name->span,
          std::format("methods cannot be attached to error type '{}'",
                      fn.type_name->name));
    return;
  }
  if (!sinfo.origin_package.empty() &&
      sinfo.origin_package != current_package_name()) {
    error(fn.type_name->span,
          std::format("cannot bind a type method to type '{}' from another "
                      "package",
                      fn.type_name->name));
    return;
  }
  auto &vec = struct_type_methods_[type_sym->type.get()];
  for (auto &m : vec)
    if (m.name == std::string(fn.name.name)) {
      error(fn.name.span,
            std::format("type method '{}.{}' is already declared",
                        fn.type_name->name, fn.name.name));
      return;
    }
  vec.push_back({std::string(fn.name.name), fn_type, fn.is_public,
                 current_package_name()});
}

TypePtr Analyzer::lookup_struct_type_method(const TypePtr &struct_type,
                                            const std::string &name) const {
  if (!struct_type)
    return nullptr;
  auto it = struct_type_methods_.find(struct_type.get());
  if (it == struct_type_methods_.end())
    return nullptr;
  for (auto &m : it->second)
    if (m.name == name)
      return m.signature;
  return nullptr;
}

void Analyzer::resolve_struct_decl(const StructDeclNode &s) {
  std::vector<FieldInfo> fields;
  std::vector<MethodInfo> methods;
  std::vector<TypeParam> type_params;

  // If the struct is generic, push a temporary scope for type params.
  bool has_generics = s.generic.has_value();
  if (has_generics) {
    push_scope(ScopeKind::Block);
    type_params = enter_generics(*s.generic);
  }

  std::string struct_name(s.name.name);
  auto &target_scope = has_generics ? current_scope->parent : current_scope;

  // Publish the type before resolving fields, so a mention of the struct
  // inside its own body resolves to this very type rather than the Invalid
  // sentinel. The remaining detail is filled in below; every holder shares
  // the one TypePtr and sees it complete.
  auto struct_type = make_struct_type(struct_name, {}, std::move(methods),
                                      type_params, current_package_name());
  auto sym_it = target_scope->symbols.find(struct_name);
  if (sym_it != target_scope->symbols.end())
    sym_it->second.type = struct_type;

  resolving_structs_.insert(struct_name);

  // Resolve fields.  Methods are bound externally (`fn (x T) M()`) and
  // registered onto the struct type by resolve_func_decl.
  for (auto &member : s.members) {
    auto *fs = std::get_if<FieldSpecNode>(&member.member->data);
    if (!fs)
      continue;
    auto ft = resolve_type(*fs->type);
    for (auto &ident : fs->names.identifiers) {
      fields.push_back({std::string(ident.name), ft, member.is_public,
                        fs->default_value.get()});
    }
  }

  // Resolve embeds. Each entry is an IdentifierNode (local) or a
  // SelectorNode (`pkg.Name`); resolve_type handles both, so we get
  // qualified-name support for free.
  std::vector<TypePtr> embeds;
  for (auto &embed_node : s.embeds) {
    auto et = resolve_type(*embed_node);
    if (!et || et->kind == TypeKind::Invalid) continue;
    if (et->kind != TypeKind::Struct) {
      error(embed_node->span, "embedded type must be a struct");
      continue;
    }
    if (embed_name_taken(*embed_node, et, fields, embeds)) continue;
    embeds.push_back(et);
  }

  resolving_structs_.erase(struct_name);

  auto &info = std::get<StructTypeInfo>(struct_type->detail);
  info.fields = std::move(fields);
  info.embeds = std::move(embeds);

  if (has_generics) {
    pop_scope();
  }
}

// resolve_error_decl — a nominal error is struct-backed with the `is_error`
// marker, boxed at runtime as a pointer to { type_id, message, ...fields }.
// Field 0 is the hidden `type_id`; field 1 the mandatory `message` string
// (auto-injected; the body supplies only its optional `message = Expr`
// default); extra fields follow.
void Analyzer::resolve_error_decl(const ErrorDeclNode &e) {
  std::string error_name(e.name.name);
  auto err_type = make_struct_type(error_name, {}, /*methods=*/{},
                                   /*type_params=*/{}, current_package_name());
  std::get<StructTypeInfo>(err_type->detail).is_error = true;

  auto sym_it = current_scope->symbols.find(error_name);
  if (sym_it != current_scope->symbols.end())
    sym_it->second.type = err_type;

  std::vector<FieldInfo> fields;
  fields.push_back({"type_id", builtins.int64_type, /*is_public=*/false,
                    nullptr});
  fields.push_back({"message", builtins.string_type, /*is_public=*/true,
                    e.message_default.get()});

  for (auto &member : e.members) {
    auto *fs = std::get_if<FieldSpecNode>(&member.member->data);
    if (!fs)
      continue;
    auto ft = resolve_type(*fs->type);
    for (auto &ident : fs->names.identifiers)
      fields.push_back({std::string(ident.name), ft, member.is_public,
                        fs->default_value.get()});
  }

  std::get<StructTypeInfo>(err_type->detail).fields = std::move(fields);
}

std::optional<std::string> plain_string_literal(const NodePtr &n) {
  if (!n)
    return std::nullopt;
  auto *sl = std::get_if<StringLiteralNode>(&n->data);
  if (!sl || sl->fragments.size() != 1)
    return std::nullopt;
  auto *frag = std::get_if<StringFragmentNode>(&sl->fragments[0]->data);
  if (!frag)
    return std::nullopt;
  return unescape_string_fragment(*frag);
}

void Analyzer::resolve_enum_decl(const EnumDeclNode &e) {
  std::vector<EnumVariant> variants;
  int64_t next_index = 0;
  for (auto &field : e.fields) {
    int64_t index = next_index;
    std::string string_value;
    if (e.string_backed) {
      string_value = std::string(field.name.name);
      if (auto s = plain_string_literal(field.value))
        string_value = *s;
    } else if (field.value) {
      if (auto *lit = std::get_if<IntegerLiteralNode>(&field.value->data)) {
        std::string clean;
        for (char c : lit->literal)
          if (c != '_') clean += c;
        index = std::stoll(clean);
      }
    }
    variants.push_back(
        {std::string(field.name.name), index, std::move(string_value)});
    next_index = index + 1;
  }

  auto enum_type =
      make_enum_type(std::string(e.name.name), std::move(variants),
                     current_package_name(), e.string_backed);

  auto sym_it = current_scope->symbols.find(std::string(e.name.name));
  if (sym_it != current_scope->symbols.end()) {
    sym_it->second.type = enum_type;
  }
}

void Analyzer::resolve_interface_decl(const InterfaceDeclNode &i) {
  std::vector<TypeParam> type_params;

  bool has_generics = i.generic.has_value();
  if (has_generics) {
    push_scope(ScopeKind::Block);
    type_params = enter_generics(*i.generic);
  }

  // Forward-declare the interface type so its methods can reference the
  // interface itself (Go-style self-referential signatures, e.g.
  // Equals(Hashable) Bool inside `interface Hashable`).  The method list is
  // filled in after signature resolution.
  auto iface_type = make_interface_type(
      std::string(i.name.name), {}, type_params, current_package_name());

  auto &target_scope = has_generics ? current_scope->parent : current_scope;
  auto sym_it = target_scope->symbols.find(std::string(i.name.name));
  if (sym_it != target_scope->symbols.end()) {
    sym_it->second.type = iface_type;
  }

  std::vector<MethodInfo> methods;
  for (auto &field : i.methods) {
    auto fn_type = resolve_signature(field.signature);
    methods.push_back({std::string(field.name.name), fn_type, field.is_public,
                       current_package_name()});
  }

  // Patch the interface type with its resolved methods.  Embedded interfaces
  // are merged later in flatten_all_interfaces, once every interface's own
  // method set is resolved.
  std::get<InterfaceTypeInfo>(iface_type->detail).methods = std::move(methods);

  if (has_generics) {
    pop_scope();
  }
}

void Analyzer::flatten_all_interfaces(
    const std::vector<const InterfaceDeclNode *> &ifaces) {
  std::unordered_map<std::string, const InterfaceDeclNode *> by_name;
  for (auto *i : ifaces)
    by_name[std::string(i->name.name)] = i;

  std::unordered_map<std::string, int> state; // 0 unvisited, 1 active, 2 done
  for (auto *i : ifaces)
    flatten_interface(*i, by_name, state);
}

// Merge each embedded interface's method set into `decl`'s own.  Local embeds
// are flattened first (depth-first) so transitive methods are carried through;
// imported embeds already arrive flat from their `.sgi`.
void Analyzer::flatten_interface(
    const InterfaceDeclNode &decl,
    const std::unordered_map<std::string, const InterfaceDeclNode *> &by_name,
    std::unordered_map<std::string, int> &state) {
  std::string key(decl.name.name);
  if (state[key] == 2)
    return;
  if (state[key] == 1) {
    error(decl.span,
          std::format("interface '{}' is part of an embedding cycle", key));
    state[key] = 2;
    return;
  }
  state[key] = 1;

  auto sym = lookup(key);
  if (sym && sym->type && sym->type->kind == TypeKind::Interface) {
    auto &info = std::get<InterfaceTypeInfo>(sym->type->detail);
    for (auto &embed_node : decl.embeds)
      merge_embed(info, *embed_node, by_name, state);
  }

  state[key] = 2;
}

// Resolve a single embedded name to an interface and fold its methods in,
// recursing first when the embed names another local interface.
void Analyzer::merge_embed(
    InterfaceTypeInfo &info, const Node &embed_node,
    const std::unordered_map<std::string, const InterfaceDeclNode *> &by_name,
    std::unordered_map<std::string, int> &state) {
  if (auto *id = std::get_if<IdentifierNode>(&embed_node.data)) {
    auto it = by_name.find(std::string(id->name));
    if (it != by_name.end())
      flatten_interface(*it->second, by_name, state);
  }

  TypePtr embedded = resolve_type(embed_node);
  if (!embedded || is_invalid_type(embedded))
    return;
  if (embedded->kind != TypeKind::Interface) {
    error(embed_node.span,
          std::format("embedded type '{}' is not an interface",
                      type_to_string(embedded)));
    return;
  }
  merge_embedded_methods(info, embedded, embed_node);
}

void Analyzer::merge_embedded_methods(InterfaceTypeInfo &target,
                                      const TypePtr &embedded,
                                      const Node &embed_node) {
  auto &source = std::get<InterfaceTypeInfo>(embedded->detail);
  for (auto &method : source.methods) {
    auto existing =
        std::find_if(target.methods.begin(), target.methods.end(),
                     [&](const MethodInfo &m) { return m.name == method.name; });
    if (existing == target.methods.end()) {
      target.methods.push_back(method);
      continue;
    }
    if (!types_equal(existing->signature, method.signature))
      error(embed_node.span,
            std::format("embedded method '{}' conflicts with an existing "
                        "method of the same name",
                        method.name));
  }
}

void Analyzer::resolve_const_decl(const ConstDeclNode &c) {
  TypePtr const_type = nullptr;
  if (c.type) {
    const_type = resolve_type(**c.type);
  }

  // The initializer expression will be type-checked later; for now we just
  // record the declared type if present.
  auto sym_it = current_scope->symbols.find(std::string(c.name.name));
  if (sym_it != current_scope->symbols.end() && const_type) {
    sym_it->second.type = const_type;
  }
}

// type ID T      — nominal: distinct identity, inherits + shadows methods.
// type ID = T    — structural: transparent to the underlying, no own methods.
void Analyzer::resolve_type_decl(const TypeDeclNode &t) {
  auto underlying = resolve_type(*t.underlying);
  auto sym_it = current_scope->symbols.find(std::string(t.name.name));
  if (sym_it == current_scope->symbols.end())
    return;
  sym_it->second.kind = SymbolKind::Type;
  sym_it->second.type =
      make_alias_type(std::string(t.name.name), underlying, {},
                      current_package_name(), t.is_structural);
}

// ===========================================================================
// Phase 3 — Name resolution in function/method bodies
// ===========================================================================

// Helper: resolve names inside a function declaration body.  Called from
// visit_source after all declarations have been resolved.
void Analyzer::resolve_func_decl_body(const FuncDeclNode &fn) {
  // Extern declarations have no body to resolve.
  if (fn.is_extern)
    return;
  // A parse error can leave a non-extern function without a body.
  if (!fn.body)
    return;

  // Generic functions are analysed lazily, once per instantiation.
  // Receiver methods on generic receiver types (Array/Map) still flow
  // through the normal path because their T is the element type.
  if (fn.generic) {
    if (!fn.receiver)
      return;
    auto &rt = fn.receiver->type->data;
    bool is_generic_recv = std::get_if<ArrayTypeNode>(&rt) ||
                           std::get_if<MapTypeNode>(&rt);
    if (!is_generic_recv)
      return;
  }

  push_scope(ScopeKind::Function);

  // Enter generics if present.
  if (fn.generic) {
    enter_generics(*fn.generic);
  }

  // Declare the receiver if present.
  if (fn.receiver) {
    auto recv_type = resolve_type(*fn.receiver->type);
    declare_local(Symbol::parameter(std::string(fn.receiver->name.name),
                                    recv_type, fn.receiver->name.span));
  }

  // Set return types on the function scope.
  if (fn.signature.return_type)
    current_scope->return_types.push_back(resolve_type(*fn.signature.return_type));

  // Declare parameters.
  declare_parameters(fn.signature);

  // Resolve names in the body.
  auto &block = std::get<BlockNode>(fn.body->data);
  resolve_block(block);

  pop_resolve_scope();
}

// ===========================================================================
// Expression name resolution
// ===========================================================================

void Analyzer::resolve_expr(const Node &node) {
  std::visit(
      overloaded{
          [&](const IdentifierNode &n) {
            resolve_identifier(n, node, NameUse::Read);
          },
          [&](const BoolLiteralNode &) { /* leaf — nothing to resolve */ },
          [&](const EnumShorthandNode &) { /* leaf — resolved in check */ },
          [&](const IntegerLiteralNode &) { /* leaf */ },
          [&](const FloatLiteralNode &) { /* leaf */ },
          [&](const StringLiteralNode &n) { resolve_string_literal(n); },
          [&](const StringFragmentNode &) { /* leaf */ },
          [&](const ArrayLiteralNode &n) { resolve_array_literal(n); },
          [&](const MapLiteralNode &n) { resolve_map_literal(n); },
          [&](const StructLiteralNode &n) { resolve_struct_literal(n); },
          [&](const BinaryExprNode &n) { resolve_binary_expr(n); },
          [&](const UnaryExprNode &n) { resolve_unary_expr(n); },
          [&](const IsExpr &n) { resolve_expr(*n.value); },
          [&](const GroupExprNode &n) { resolve_group_expr(n); },
          [&](const CallExprNode &n) { resolve_call_expr(n); },
          [&](const IndexExprNode &n) { resolve_index_expr(n); },
          [&](const SelectorNode &n) { resolve_selector(n); },
          [&](const IfExprNode &n) { resolve_if_expr(n); },
          [&](const SwitchExprNode &n) { resolve_switch_expr(n); },
          [&](const ForExprNode &n) { resolve_for_expr(n); },
          [&](const SpawnExprNode &n) { resolve_spawn_expr(n, node); },
          [&](const OrExprNode &n) { resolve_or_expr(n); },
          [&](const FuncExprNode &n) { resolve_func_expr(n, node); },
          [&](const ImportExprNode &) { /* processed during import phase */ },
          [&](const BlockNode &n) {
            push_scope(ScopeKind::Block);
            resolve_block(n);
            pop_resolve_scope();
          },
          // Statements that can appear as expressions in blocks.
          [&](const VarDeclNode &n) { resolve_var_decl(n, node); },
          [&](const DeclAssignNode &n) { resolve_decl_assign(n, node); },
          [&](const AssignNode &n) { resolve_assign(n); },
          [&](const ReturnNode &n) { resolve_return(n); },
          [&](const BreakNode &n) { resolve_break(n); },
          [&](const NextNode &) { /* nothing to resolve */ },
          [&](const IncrementNode &n) { resolve_increment(n); },
          [&](const DecrementNode &n) { resolve_decrement(n); },
          [&](const auto &) {
            // Type nodes, structural nodes, etc. — nothing to resolve here.
          },
      },
      node.data);
}

void Analyzer::resolve_identifier(const IdentifierNode &ident,
                                  const Node &parent, NameUse use) {
  std::string name(ident.name);

  // Ignored identifiers (starting with _) don't need resolution.
  if (is_ignored_name(name))
    return;

  auto sym = lookup(name);
  if (!sym) {
    undefined_error(ident.span, name);
    return;
  }
  record_symbol(parent, *sym);
  if (use == NameUse::Read)
    current_scope->mark_read(name);

  // ── Capture detection for closures ─────────────────────────────────
  // If this symbol is a local variable/parameter and we're inside a closure,
  // check if it was declared outside the closure boundary.
  if (sym->kind == SymbolKind::Variable || sym->kind == SymbolKind::Parameter) {
    // Walk from current_scope outward looking for a closure boundary.
    // If we find one before we find the symbol's declaration scope,
    // the symbol is captured by that closure.
    auto scope = current_scope;
    while (scope) {
      // If the symbol is declared in this scope, it's local — not captured.
      if (scope->lookup_local(name))
        break;
      // If we cross a closure boundary, this variable is captured.
      if (scope->is_closure) {
        // Record the capture on the closure's pending list.
        // (pending_closure_captures is set when resolving a FuncExprNode)
        if (pending_closure_node_) {
          auto &caps = current_instantiation_
                           ? current_instantiation_
                                 ->node_captures[pending_closure_node_]
                           : node_captures[pending_closure_node_];
          // Avoid duplicate captures.
          bool already = false;
          for (auto &c : caps)
            if (c.name == name) {
              already = true;
              break;
            }
          if (!already)
            caps.push_back({name, sym->type});
        }
        break;
      }
      scope = scope->parent;
    }
  }

  // ── Capture detection for spawn blocks ────────────────────────────
  // If this symbol is a local variable/parameter and we're inside a
  // spawn block, check if it was declared outside the spawn boundary.
  if ((sym->kind == SymbolKind::Variable ||
       sym->kind == SymbolKind::Parameter) &&
      pending_spawn_node_) {
    auto scope = current_scope;
    while (scope) {
      if (scope->lookup_local(name))
        break;
      if (scope->kind == ScopeKind::Spawn) {
        auto &caps = current_instantiation_
                         ? current_instantiation_
                               ->spawn_captures[pending_spawn_node_]
                         : spawn_captures[pending_spawn_node_];
        bool already = false;
        for (auto &c : caps)
          if (c.name == name) {
            already = true;
            break;
          }
        if (!already)
          caps.push_back({name, sym->type, SpawnCaptureKind::Copy});
        break;
      }
      scope = scope->parent;
    }
  }
}

void Analyzer::resolve_block(const BlockNode &block) {
  for (auto &stmt : block.stmts) {
    resolve_block_stmt(*stmt);
  }
}

void Analyzer::resolve_block_stmt(const Node &node) {
  // Dispatch: some nodes are statements that introduce names,
  // others are expressions.
  std::visit(overloaded{
                 [&](const VarDeclNode &n) { resolve_var_decl(n, node); },
                 [&](const DeclAssignNode &n) { resolve_decl_assign(n, node); },
                 [&](const AssignNode &n) { resolve_assign(n); },
                 [&](const IncrementNode &n) { resolve_increment(n); },
                 [&](const DecrementNode &n) { resolve_decrement(n); },
                 [&](const ReturnNode &n) { resolve_return(n); },
                 [&](const BreakNode &n) { resolve_break(n); },
                 [&](const NextNode &) { /* nothing */ },
                 [&](const auto &) {
                   // Everything else is an expression.
                   resolve_expr(node);
                 },
             },
             node.data);
}

// ── Expression sub-resolvers ───────────────────────────────────────────

void Analyzer::resolve_call_expr(const CallExprNode &node) {
  resolve_expr(*node.callee);
  for (auto &arg : node.args) {
    resolve_expr(*arg);
  }
}

void Analyzer::resolve_index_expr(const IndexExprNode &node) {
  resolve_expr(*node.object);
  resolve_expr(*node.index);
}

void Analyzer::resolve_selector(const SelectorNode &node) {
  // Resolve the left-hand side; the field name (.field) is resolved later
  // during type-checking against the object's type.
  resolve_expr(*node.object);

  // For module selectors, resolve the member in the module's scope
  // to provide early name-resolution feedback.
  if (auto *ident = std::get_if<IdentifierNode>(&node.object->data)) {
    auto sym = lookup(std::string(ident->name));
    if (sym && sym->kind == SymbolKind::Module && sym->type &&
        sym->type->kind == TypeKind::Module) {
      auto &mod = std::get<ModuleTypeInfo>(sym->type->detail);
      std::string field_name(node.field.name);
      bool found = false;
      for (auto &exp : mod.exports) {
        if (exp.name == field_name) {
          found = true;
          break;
        }
      }
      if (!found) {
        error(node.field.span,
              std::format("package '{}' has no exported member '{}'", mod.name,
                          field_name));
      }
    }
  }
}

void Analyzer::resolve_binary_expr(const BinaryExprNode &node) {
  resolve_expr(*node.lhs);
  resolve_expr(*node.rhs);
}

void Analyzer::resolve_unary_expr(const UnaryExprNode &node) {
  resolve_expr(*node.operand);
}

void Analyzer::resolve_group_expr(const GroupExprNode &node) {
  resolve_expr(*node.inner);
}

void Analyzer::resolve_string_literal(const StringLiteralNode &node) {
  for (auto &frag : node.fragments) {
    // StringFragmentNode is a leaf; interpolated expressions need resolution.
    if (!std::holds_alternative<StringFragmentNode>(frag->data)) {
      resolve_expr(*frag);
    }
  }
}

void Analyzer::resolve_array_literal(const ArrayLiteralNode &node) {
  for (auto &elem : node.elements) {
    resolve_expr(*elem);
  }
}

void Analyzer::resolve_map_literal(const MapLiteralNode &node) {
  for (auto &entry : node.entries) {
    resolve_expr(*entry.key);
    resolve_expr(*entry.value);
  }
}

void Analyzer::resolve_struct_literal(const StructLiteralNode &node) {
  // Resolve the type expression (e.g. Point, pkg.Type).
  resolve_expr(*node.type_expr);
  // Resolve field value expressions.
  for (auto &field : node.fields) {
    resolve_expr(*field.value);
  }
}

void Analyzer::resolve_if_expr(const IfExprNode &node) {
  resolve_expr(*node.condition);

  push_scope(ScopeKind::Block);
  auto &then_block = std::get<BlockNode>(node.then_block->data);
  resolve_block(then_block);
  pop_resolve_scope();

  if (node.else_block) {
    push_scope(ScopeKind::Block);
    auto &else_block = std::get<BlockNode>((*node.else_block)->data);
    resolve_block(else_block);
    pop_resolve_scope();
  }
}

void Analyzer::resolve_switch_expr(const SwitchExprNode &node) {
  resolve_expr(*node.subject);
  for (auto &arm : node.arms) {
    for (auto &pat : arm.patterns)
      resolve_expr(*pat);
    // The body may be an expression or a block.
    if (auto *block = std::get_if<BlockNode>(&arm.body->data)) {
      push_scope(ScopeKind::Block);
      resolve_block(*block);
      pop_resolve_scope();
    } else {
      resolve_expr(*arm.body);
    }
  }
  if (node.else_body) {
    if (auto *block = std::get_if<BlockNode>(&(*node.else_body)->data)) {
      push_scope(ScopeKind::Block);
      resolve_block(*block);
      pop_resolve_scope();
    } else {
      resolve_expr(**node.else_body);
    }
  }
}

void Analyzer::resolve_for_expr(const ForExprNode &node) {
  push_scope(ScopeKind::Loop);

  // Resolve the mode (condition, range clause, or iter clause).
  if (node.mode) {
    std::visit(overloaded{
                   [&](const ForRangeClauseNode &range) {
                     // Resolve the iterable expression first.
                     resolve_expr(*range.iterable);
                     // Declare the loop variable(s) into the loop scope.
                     for (auto &var : range.vars) {
                       declare_local(Symbol::variable(std::string(var.name),
                                                      nullptr, var.span));
                     }
                   },
                   [&](const ForIterClauseNode &iter) {
                     // Init: typically a VarDecl or DeclAssign.
                     resolve_block_stmt(*iter.init);
                     // Condition.
                     resolve_expr(*iter.condition);
                     // Update.
                     resolve_block_stmt(*iter.update);
                   },
                   [&](const auto &) {
                     // Bare condition expression.
                     resolve_expr(**node.mode);
                   },
               },
               (*node.mode)->data);
  }

  // Declare the accumulator pipe if present.
  if (node.accumulator) {
    std::string acc(node.accumulator->name);
    declare_local(Symbol::variable(acc, nullptr, node.accumulator->span));
    // The loop's value is the accumulator, so the expression reads it even
    // when the body only assigns to it — as `|acc| { acc += x }` does.
    current_scope->mark_read(acc);
  }

  // Resolve the body.
  auto &body_block = std::get<BlockNode>(node.body->data);
  resolve_block(body_block);

  pop_resolve_scope();
}

void Analyzer::resolve_spawn_expr(const SpawnExprNode &node,
                                  const Node &parent) {
  // Resolve the channel element type (|T| in `|T| spawn ...`) while the
  // surrounding scope still has user types like structs visible.  Codegen
  // reads this map to avoid redoing the lookup after the scope is popped.
  if (node.generic && !node.generic->type_params.empty()) {
    auto ch_elem = resolve_type(*node.generic->type_params[0]);
    if (current_instantiation_) {
      current_instantiation_->spawn_channel_elem_types[&parent] =
          std::move(ch_elem);
    } else {
      spawn_channel_elem_types[&parent] = std::move(ch_elem);
    }
  }

  push_scope(ScopeKind::Spawn);

  // Push this spawn onto the stack for capture tracking.
  spawn_node_stack_.push_back(&parent);
  pending_spawn_node_ = &parent;

  // Declare the pipe variable (task context) if present.
  if (node.pipe) {
    declare_local(Symbol::variable(std::string(node.pipe->name),
                                   builtins.context_type, node.pipe->span));
  }

  // Body can be a block or a single identifier (function reference).
  if (auto *block = std::get_if<BlockNode>(&node.body->data)) {
    resolve_block(*block);
  } else {
    resolve_expr(*node.body);
  }

  // Pop the spawn stack.
  spawn_node_stack_.pop_back();
  pending_spawn_node_ =
      spawn_node_stack_.empty() ? nullptr : spawn_node_stack_.back();

  // Classify spawn captures based on their types.
  auto &caps_map =
      current_instantiation_ ? current_instantiation_->spawn_captures
                             : spawn_captures;
  auto cap_it = caps_map.find(&parent);
  if (cap_it != caps_map.end()) {
    for (auto &cap : cap_it->second) {
      if (!cap.type) {
        cap.kind = SpawnCaptureKind::Copy;
        continue;
      }
      // Refcounted types (String, Array, Map) use Share (COW).
      // Everything else (scalars, structs) is trivially copied.
      switch (cap.type->kind) {
      case TypeKind::String:
      case TypeKind::Array:
      case TypeKind::Map:
        cap.kind = SpawnCaptureKind::Share;
        break;
      default:
        cap.kind = SpawnCaptureKind::Copy;
        break;
      }
    }
  }

  pop_resolve_scope();
}

void Analyzer::resolve_or_expr(const OrExprNode &node) {
  resolve_expr(*node.expr);

  push_scope(ScopeKind::Block);

  // Declare the error pipe if present.
  if (node.pipe) {
    declare_local(Symbol::variable(std::string(node.pipe->name),
                                   builtins.error_base, node.pipe->span));
  }

  auto &block = std::get<BlockNode>(node.fallback->data);
  resolve_block(block);

  pop_resolve_scope();
}

void Analyzer::resolve_func_expr(const FuncExprNode &node, const Node &parent) {
  push_scope(ScopeKind::Function);
  current_scope->is_closure = true;

  // Push this closure onto the stack for capture tracking.
  closure_node_stack_.push_back(&parent);
  pending_closure_node_ = &parent;

  if (node.generic) {
    enter_generics(*node.generic);
  }

  // Set return types.
  if (node.signature.return_type)
    current_scope->return_types.push_back(resolve_type(*node.signature.return_type));

  declare_parameters(node.signature);

  auto &block = std::get<BlockNode>(node.body->data);
  resolve_block(block);

  // Pop closure tracking state.
  closure_node_stack_.pop_back();
  pending_closure_node_ =
      closure_node_stack_.empty() ? nullptr : closure_node_stack_.back();

  pop_resolve_scope();
}

// ===========================================================================
// Phase 4 — Statement name resolution
// ===========================================================================

void Analyzer::resolve_stmt(const Node &node) { resolve_block_stmt(node); }

void Analyzer::resolve_var_decl(const VarDeclNode &var,
                                const Node & /*parent*/) {
  // Resolve the type annotation if present.
  if (var.type) {
    resolve_type(**var.type);
  }
  // Resolve the initializer if present.
  if (var.init) {
    resolve_expr(**var.init);
  }
  // Declare the variable — after resolving the init, so the name can't
  // refer to itself during initialization.
  declare_local(
      Symbol::variable(std::string(var.name.name), nullptr, var.name.span));
}

void Analyzer::resolve_decl_assign(const DeclAssignNode &decl,
                                   const Node & /*parent*/) {
  // Resolve the RHS first (before declaring LHS names).
  resolve_expr(*decl.value);
  // Declare each target name.
  for (auto &ident : decl.targets.identifiers) {
    declare_local(
        Symbol::variable(std::string(ident.name), nullptr, ident.span));
  }
}

void Analyzer::resolve_write_target(const Node &target) {
  if (auto *ident = std::get_if<IdentifierNode>(&target.data)) {
    resolve_identifier(*ident, target, NameUse::Write);
    return;
  }
  resolve_expr(target);
}

void Analyzer::resolve_assign(const AssignNode &node) {
  for (auto &target : node.targets) {
    resolve_write_target(*target);
  }
  for (auto &value : node.values) {
    resolve_expr(*value);
  }
}

void Analyzer::resolve_return(const ReturnNode &node) {
  if (!current_scope->is_inside(ScopeKind::Function)) {
    error(node.span, "'return' outside of function");
  }
  if (node.value)
    resolve_expr(*node.value);
}

void Analyzer::resolve_break(const BreakNode &node) {
  if (!current_scope->is_inside(ScopeKind::Loop)) {
    error(node.span, "'break' outside of loop");
  }
  for (auto &val : node.values) {
    resolve_expr(*val);
  }
}

void Analyzer::resolve_increment(const IncrementNode &node) {
  resolve_write_target(*node.operand);
}

void Analyzer::resolve_decrement(const DecrementNode &node) {
  resolve_write_target(*node.operand);
}

// ===========================================================================
// Phase 4 — Type-check function/method bodies
// ===========================================================================

void Analyzer::check_func_decl_body(const FuncDeclNode &fn) {
  // Extern declarations have no body to type-check.
  if (fn.is_extern)
    return;
  // A parse error can leave a non-extern function without a body.
  if (!fn.body)
    return;

  // Generic functions are type-checked lazily per instantiation.
  // Receiver methods on generic receiver types (Array/Map) still flow
  // through the eager path because their T is the element type.
  bool is_eager_kind_method = false;
  if (fn.generic) {
    if (!fn.receiver)
      return;
    auto &rt = fn.receiver->type->data;
    bool is_generic_recv = std::get_if<ArrayTypeNode>(&rt) ||
                           std::get_if<MapTypeNode>(&rt);
    if (!is_generic_recv)
      return;
    is_eager_kind_method = true;
  }

  // Mark the current kind_methods_ body so resolve_method_signature can
  // record TypeParam protocol dispatch usage.  Restored on exit.
  const FuncDeclNode *saved_eager_kind = current_eager_kind_method_decl_;
  if (is_eager_kind_method)
    current_eager_kind_method_decl_ = &fn;

  push_scope(ScopeKind::Function);

  if (fn.generic)
    enter_generics(*fn.generic);

  if (fn.receiver) {
    auto recv_type = resolve_type(*fn.receiver->type);
    declare_local(Symbol::parameter(std::string(fn.receiver->name.name),
                                    recv_type, fn.receiver->name.span));
  }

  if (fn.signature.return_type)
    current_scope->return_types.push_back(resolve_type(*fn.signature.return_type));

  declare_parameters(fn.signature);

  auto &block = std::get<BlockNode>(fn.body->data);
  // Tail-position for-expression with accumulator: pass the function's
  // declared return type as the accumulator hint so the body of
  // `for i : xs |acc| { acc += i }` typechecks against the return.
  TypePtr tail_hint;
  if (current_scope->return_types.size() == 1 &&
      !types_equal(current_scope->return_types[0], builtins.void_type) &&
      !block.stmts.empty() &&
      std::get_if<ForExprNode>(&block.stmts.back()->data))
    tail_hint = current_scope->return_types[0];

  bool tail_shorthand =
      current_scope->return_types.size() == 1 && !block.stmts.empty() &&
      std::get_if<EnumShorthandNode>(&block.stmts.back()->data);

  TypePtr body_type;
  if (tail_hint) {
    for (size_t i = 0; i + 1 < block.stmts.size(); ++i)
      check_expr(*block.stmts[i]);
    auto &for_node = std::get<ForExprNode>(block.stmts.back()->data);
    body_type = check_for_expr(for_node, tail_hint);
    record_type(*block.stmts.back(), body_type);
  } else if (tail_shorthand) {
    for (size_t i = 0; i + 1 < block.stmts.size(); ++i)
      check_expr(*block.stmts[i]);
    body_type = check_expr_expecting(*block.stmts.back(),
                                     current_scope->return_types[0]);
  } else {
    body_type = check_block(block);
  }

  // Check that the tail expression matches the return type (if non-Void).
  // If the last statement always returns (directly or through branches),
  // the return values were already checked by check_return.
  if (!current_scope->return_types.empty() && !block.stmts.empty()) {
    bool tail_is_return = always_returns(*block.stmts.back());
    if (!tail_is_return) {
      auto &expected = current_scope->return_types;
      if (expected.size() == 1 && !is_invalid_type(body_type)) {
        if (!types_equal(expected[0], builtins.void_type)) {
          expect_assignable(fn.body->span, expected[0], body_type,
                            "return type");
        }
      }
    }
  }

  pop_scope();
  current_eager_kind_method_decl_ = saved_eager_kind;
}

} // namespace saga

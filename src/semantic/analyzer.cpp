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

} // namespace saga

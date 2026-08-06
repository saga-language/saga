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
TypePtr normalize_generic_receiver_sig(const TypePtr &t,
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

} // namespace saga

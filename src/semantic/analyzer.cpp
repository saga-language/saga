// Copywrite 2026 Rob Thornton
// SPDX-License-Identifier: MIT

#include "semantic/analyzer.hpp"
#include "semantic/sgi.hpp"
#include "frontend/parser.hpp"
#include "util/internal_error.hpp"

#include <algorithm>
#include <charconv>
#include <filesystem>
#include <format>

namespace saga {

namespace {

// Methods whose body passes the receiver as the first argument to one of
// these C runtime functions mutate the caller's collection in place, so they
// cannot be applied to an immutable constant.  Value-returning copy-on-write
// methods (Append/Insert/Set) are absent: they never write through the
// receiver, so they are valid on a const (the result is a fresh array).
const std::unordered_set<std::string> kMutatingIntrinsics{
    "saga_array_pop", "saga_map_set", "saga_map_remove"};

bool is_kind_method_mutating(const FuncDeclNode &fn) {
  if (!fn.body || !fn.receiver) return false;
  auto *blk = std::get_if<BlockNode>(&fn.body->data);
  if (!blk) return false;
  std::string_view recv = fn.receiver->name.name;

  for (auto &stmt : blk->stmts) {
    auto *call = std::get_if<CallExprNode>(&stmt->data);
    if (!call) continue;
    auto *id = std::get_if<IdentifierNode>(&call->callee->data);
    if (!id) continue;
    if (!kMutatingIntrinsics.count(std::string(id->name))) continue;
    if (call->args.empty()) continue;
    auto *recv_id = std::get_if<IdentifierNode>(&call->args[0]->data);
    if (recv_id && recv_id->name == recv) return true;
  }
  return false;
}

} // namespace

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

bool Analyzer::declare(const Symbol &sym) {
  if (!current_scope->declare(sym)) {
    redeclaration_error(sym.decl_span, sym.name);
    return false;
  }
  return true;
}

bool Analyzer::declare_local(const Symbol &sym) {
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
              [&](const ImportDeclNode &imp) { check_import_decl(imp); },
              [&](const auto &) {},
          },
          decl->data);
    }
  }

  pop_scope();
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
                   [&](const ImportDeclNode &imp) { check_import_decl(imp); },
                   [&](const auto &) {},
               },
               decl->data);
  }

  pop_scope();
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

    // Rule 2 — the composed set must be unique. A union alternative is spliced,
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

TypePtr Analyzer::resolve_signature(const SignatureNode &sig) {
  std::vector<TypePtr> params;
  for (auto &p : sig.params) {
    auto pt = resolve_type(*p.type);
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

// A plain (non-interpolated) string literal's text, else nullopt.
static std::optional<std::string> plain_string_literal(const NodePtr &n) {
  if (!n)
    return std::nullopt;
  auto *sl = std::get_if<StringLiteralNode>(&n->data);
  if (!sl || sl->fragments.size() != 1)
    return std::nullopt;
  auto *frag = std::get_if<StringFragmentNode>(&sl->fragments[0]->data);
  if (!frag)
    return std::nullopt;
  return unescape_string_fragment(frag->text);
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

  pop_scope();
}

// ===========================================================================
// Expression name resolution
// ===========================================================================

void Analyzer::resolve_expr(const Node &node) {
  std::visit(
      overloaded{
          [&](const IdentifierNode &n) { resolve_identifier(n, node); },
          [&](const BoolLiteralNode &) { /* leaf — nothing to resolve */ },
          [&](const NullLiteralNode &) { /* leaf — nothing to resolve */ },
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
            pop_scope();
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
                                  const Node &parent) {
  std::string name(ident.name);

  // Ignored identifiers (starting with _) don't need resolution.
  if (!name.empty() && name[0] == '_')
    return;

  auto sym = lookup(name);
  if (!sym) {
    undefined_error(ident.span, name);
    return;
  }
  record_symbol(parent, *sym);

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
  pop_scope();

  if (node.else_block) {
    push_scope(ScopeKind::Block);
    auto &else_block = std::get<BlockNode>((*node.else_block)->data);
    resolve_block(else_block);
    pop_scope();
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
      pop_scope();
    } else {
      resolve_expr(*arm.body);
    }
  }
  if (node.else_body) {
    if (auto *block = std::get_if<BlockNode>(&(*node.else_body)->data)) {
      push_scope(ScopeKind::Block);
      resolve_block(*block);
      pop_scope();
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
    declare_local(Symbol::variable(std::string(node.accumulator->name), nullptr,
                                   node.accumulator->span));
  }

  // Resolve the body.
  auto &body_block = std::get<BlockNode>(node.body->data);
  resolve_block(body_block);

  pop_scope();
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

  pop_scope();
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

  pop_scope();
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

  pop_scope();
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

void Analyzer::resolve_assign(const AssignNode &node) {
  for (auto &target : node.targets) {
    resolve_expr(*target);
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
  resolve_expr(*node.operand);
}

void Analyzer::resolve_decrement(const DecrementNode &node) {
  resolve_expr(*node.operand);
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

// ===========================================================================
// Expression type-checking
// ===========================================================================

TypePtr Analyzer::check_expr(const Node &node) {
  auto type = std::visit(
      overloaded{
          [&](const IdentifierNode &n) -> TypePtr {
            return check_identifier(n, node);
          },
          [&](const BoolLiteralNode &n) -> TypePtr {
            return check_bool_literal(n);
          },
          [&](const NullLiteralNode &) -> TypePtr {
            return builtins.void_type;
          },
          [&](const EnumShorthandNode &n) -> TypePtr {
            error(n.span,
                  std::format("cannot infer enum type for '.{}' here; name the "
                              "enum (Type.{}) or supply a target type",
                              n.variant.name, n.variant.name));
            return builtins.invalid_type;
          },
          [&](const IntegerLiteralNode &n) -> TypePtr {
            return check_int_literal(n);
          },
          [&](const FloatLiteralNode &n) -> TypePtr {
            return check_float_literal(n);
          },
          [&](const StringLiteralNode &n) -> TypePtr {
            return check_string_literal(n);
          },
          [&](const StringFragmentNode &) -> TypePtr {
            return builtins.string_type;
          },
          [&](const ArrayLiteralNode &n) -> TypePtr {
            return check_array_literal(n);
          },
          [&](const MapLiteralNode &n) -> TypePtr {
            return check_map_literal(n);
          },
          [&](const StructLiteralNode &n) -> TypePtr {
            return check_struct_literal(n);
          },
          [&](const BinaryExprNode &n) -> TypePtr {
            return check_binary_expr(n, node);
          },
          [&](const UnaryExprNode &n) -> TypePtr {
            return check_unary_expr(n);
          },
          [&](const IsExpr &n) -> TypePtr { return check_is_expr(n); },
          [&](const GroupExprNode &n) -> TypePtr {
            return check_group_expr(n);
          },
          [&](const CallExprNode &n) -> TypePtr {
            return check_call_expr(n, node);
          },
          [&](const IndexExprNode &n) -> TypePtr {
            return check_index_expr(n);
          },
          [&](const SelectorNode &n) -> TypePtr {
            return check_selector(n, node);
          },
          [&](const IfExprNode &n) -> TypePtr { return check_if_expr(n); },
          [&](const SwitchExprNode &n) -> TypePtr {
            return check_switch_expr(n);
          },
          [&](const ForExprNode &n) -> TypePtr { return check_for_expr(n); },
          [&](const SpawnExprNode &n) -> TypePtr {
            return check_spawn_expr(n, node);
          },
          [&](const OrExprNode &n) -> TypePtr { return check_or_expr(n); },
          [&](const FuncExprNode &n) -> TypePtr {
            return check_func_expr(n, node);
          },
          [&](const ImportExprNode &n) -> TypePtr {
            return check_import_expr(n);
          },
          [&](const BlockNode &n) -> TypePtr {
            push_scope(ScopeKind::Block);
            auto t = check_block(n);
            pop_scope();
            return t;
          },
          // Statements appearing in expression position return Void.
          [&](const VarDeclNode &n) -> TypePtr {
            check_var_decl(n, node);
            return builtins.void_type;
          },
          [&](const DeclAssignNode &n) -> TypePtr {
            check_decl_assign(n);
            return builtins.void_type;
          },
          [&](const AssignNode &n) -> TypePtr {
            check_assign(n);
            return builtins.void_type;
          },
          [&](const IncrementNode &n) -> TypePtr {
            check_increment(n);
            return builtins.void_type;
          },
          [&](const DecrementNode &n) -> TypePtr {
            check_decrement(n);
            return builtins.void_type;
          },
          [&](const ReturnNode &n) -> TypePtr {
            check_return(n);
            return builtins.void_type;
          },
          [&](const BreakNode &n) -> TypePtr {
            check_break(n);
            return builtins.void_type;
          },
          [&](const NextNode &) -> TypePtr { return builtins.void_type; },
          [&](const ArrayTypeNode &) -> TypePtr {
            return reject_type_as_value(node);
          },
          [&](const MapTypeNode &) -> TypePtr {
            return reject_type_as_value(node);
          },
          [&](const UnionTypeNode &) -> TypePtr {
            return reject_type_as_value(node);
          },
          [&](const FuncTypeNode &) -> TypePtr {
            return reject_type_as_value(node);
          },
          [&](const GenericTypeAppNode &) -> TypePtr {
            return reject_type_as_value(node);
          },
          [&](const auto &) -> TypePtr {
            return poison(node.span, "expression kind has no type rule");
          },
      },
      node.data);

  record_type(node, type);
  return type;
}

static bool is_empty_struct_shape(const TypePtr &t) {
  if (!t || t->kind != TypeKind::Struct)
    return false;
  auto &info = std::get<StructTypeInfo>(t->detail);
  return info.fields.empty() && info.embeds.empty() && info.type_params.empty();
}

static bool is_type_expr_node(const Node &node) {
  return std::holds_alternative<ArrayTypeNode>(node.data) ||
         std::holds_alternative<MapTypeNode>(node.data) ||
         std::holds_alternative<UnionTypeNode>(node.data) ||
         std::holds_alternative<FuncTypeNode>(node.data) ||
         std::holds_alternative<GenericTypeAppNode>(node.data);
}

TypePtr Analyzer::check_type_or_value_expr(const Node &node) {
  if (auto *id = std::get_if<IdentifierNode>(&node.data)) {
    auto sym = lookup(std::string(id->name));
    // A type name and a package name are both legal here and nowhere else a
    // value is expected, which is why this path exists separately from
    // check_expr.
    if (sym && (sym->kind == SymbolKind::Type ||
                sym->kind == SymbolKind::Module)) {
      record_symbol(node, *sym);
      auto type = sym->type ? sym->type : builtins.invalid_type;
      record_type(node, type);
      return type;
    }
    return check_expr(node);
  }
  if (is_type_expr_node(node)) {
    auto type = resolve_type(node);
    record_type(node, type);
    return type;
  }
  return check_expr(node);
}

TypePtr Analyzer::reject_type_as_value(const Node &node) {
  error(node.span, std::format("cannot use type '{}' as a value",
                               type_to_string(resolve_type(node))));
  return builtins.invalid_type;
}

TypePtr Analyzer::check_identifier(const IdentifierNode &ident,
                                   const Node &parent) {
  std::string name(ident.name);
  // Spec docs/language.md:25-27 — underscore-prefixed names are "ignored"
  // variables: legal to declare, illegal to read back.  Declaration sites
  // (`_x := 1`) bypass this path entirely; a use here is therefore a
  // read attempt or a re-assignment, both of which the spec forbids.
  if (!name.empty() && name[0] == '_') {
    error(ident.span,
          std::format("cannot access ignored variable '{}'", name));
    return builtins.invalid_type;
  }

  auto sym = lookup(name);
  if (!sym) {
    // Already reported during name resolution.
    return builtins.invalid_type;
  }
  record_symbol(parent, *sym);

  if (sym->kind == SymbolKind::Type) {
    if (is_empty_struct_shape(sym->type))
      return sym->type;
    error(ident.span,
          std::format("cannot use type '{}' as a value", name));
    return builtins.invalid_type;
  }

  // A package is introduced by an import binding, never copied by assignment;
  // reaching here means it was used where a value belongs. Selector objects go
  // through check_type_or_value_expr instead.
  if (sym->kind == SymbolKind::Module) {
    error(ident.span,
          std::format("cannot use package '{}' as a value; to bind it to "
                      "another name use `const Name = import \"...\"`",
                      name));
    return builtins.invalid_type;
  }

  // Forward reference inside a constant initialiser.  check_const_decl
  // runs in textual order and assigns each Constant's type as it goes,
  // so a Constant with no type means it was declared but not yet
  // checked — i.e. a sibling declared later.  Spec says this is a
  // compile-time error (docs/language.md:120-122).
  if (sym->kind == SymbolKind::Constant && !sym->type) {
    error(ident.span,
          std::format("constant '{}' read before its own declaration", name));
    return builtins.invalid_type;
  }

  return sym->type ? sym->type : builtins.invalid_type;
}

TypePtr Analyzer::check_bool_literal(const BoolLiteralNode &) {
  return builtins.bool_type;
}

TypePtr Analyzer::check_int_literal(const IntegerLiteralNode &) {
  return make_untyped_int_type();
}

TypePtr Analyzer::check_float_literal(const FloatLiteralNode &) {
  return make_untyped_float_type();
}

TypePtr Analyzer::check_string_literal(const StringLiteralNode &node) {
  // Each interpolated expression must satisfy Stringable so the runtime
  // can invoke `.String()` on it.
  for (auto &frag : node.fragments) {
    if (std::holds_alternative<StringFragmentNode>(frag->data))
      continue;
    auto t = check_expr(*frag);
    check_stringable_recursive(t, frag->span, "interpolated expression");
  }
  return builtins.string_type;
}

TypePtr Analyzer::check_array_literal(const ArrayLiteralNode &node) {
  if (node.elements.empty()) {
    // The element type comes from the context. A hole that never meets one is
    // caught where the binding is made.
    return make_array_type(builtins.unknown_type);
  }
  auto elem_type = check_expr(*node.elements[0]);
  for (size_t i = 1; i < node.elements.size(); ++i) {
    auto t = check_expr(*node.elements[i]);
    if (!is_invalid_type(t) && !is_invalid_type(elem_type)) {
      expect_assignable(node.elements[i]->span, elem_type, t, "array element");
    }
  }
  return make_array_type(elem_type);
}

TypePtr Analyzer::check_map_literal(const MapLiteralNode &node) {
  if (node.entries.empty()) {
    return make_map_type(builtins.unknown_type, builtins.unknown_type);
  }
  auto key_type = check_expr(*node.entries[0].key);
  auto val_type = check_expr(*node.entries[0].value);
  for (size_t i = 1; i < node.entries.size(); ++i) {
    auto kt = check_expr(*node.entries[i].key);
    auto vt = check_expr(*node.entries[i].value);
    if (!is_invalid_type(kt))
      expect_assignable(node.entries[i].key->span, key_type, kt, "map key");
    if (!is_invalid_type(vt))
      expect_assignable(node.entries[i].value->span, val_type, vt, "map value");
  }
  check_satisfies_protocol(key_type, ProtocolKind::Hashable,
                           node.entries[0].key->span, "map key");
  return make_map_type(key_type, val_type);
}

TypePtr Analyzer::check_struct_literal(const StructLiteralNode &node) {
  auto type_expr_type = check_type_or_value_expr(*node.type_expr);
  if (is_invalid_type(type_expr_type))
    return builtins.invalid_type;

  // For alias types, unwrap to get the underlying struct type for validation,
  // but return the alias type so the variable retains its alias identity.
  auto struct_type = type_expr_type;
  if (struct_type->kind == TypeKind::Alias) {
    struct_type = unwrap_alias(struct_type);
  }

  if (!struct_type || struct_type->kind != TypeKind::Struct) {
    error(node.type_expr->span, std::format("'{}' is not a struct type",
                                            type_to_string(type_expr_type)));
    return builtins.invalid_type;
  }

  auto &raw_info = std::get<StructTypeInfo>(struct_type->detail);

  // Pre-map field name → declared type for non-generic structs so a `.Variant`
  // field value can resolve against it. Generic field types are TypeParams,
  // which a shorthand can't resolve against, so it stays a plain check there.
  std::unordered_map<std::string, TypePtr> field_type_by_name;
  if (raw_info.type_params.empty()) {
    std::vector<FieldInfo> decl_fields;
    collect_promoted_fields(raw_info, decl_fields);
    for (auto &fi : decl_fields)
      if (fi.type)
        field_type_by_name.emplace(fi.name, fi.type);
  }

  // Check each field value first and collect types for generic inference.
  std::vector<std::pair<std::string, TypePtr>> field_vals;
  for (auto &fa : node.fields) {
    auto it = field_type_by_name.find(std::string(fa.name.name));
    TypePtr expected = it != field_type_by_name.end() ? it->second : nullptr;
    auto val_type = check_expr_expecting(*fa.value, expected);
    field_vals.push_back({std::string(fa.name.name), val_type});
  }

  // If the struct is generic, instantiate it by inferring type params.
  auto effective_type = struct_type;
  if (!raw_info.type_params.empty()) {
    effective_type =
        instantiate_generic_struct(struct_type, field_vals, node.span);
    if (is_invalid_type(effective_type))
      return builtins.invalid_type;
  }

  auto &info = std::get<StructTypeInfo>(effective_type->detail);

  // Collect all fields including those promoted from embedded types
  // (transitively). Own fields come first, so a child field shadows an
  // embedded one of the same name.
  std::vector<FieldInfo> all_fields;
  collect_promoted_fields(info, all_fields);

  // Validate each field assignment against the (possibly instantiated) type.
  for (size_t i = 0; i < field_vals.size(); ++i) {
    auto &[fname, val_type] = field_vals[i];
    bool found = false;
    for (auto &fi : all_fields) {
      if (fi.name == fname) {
        found = true;
        if (fi.type && !is_invalid_type(val_type)) {
          expect_assignable(node.span, fi.type, val_type,
                            std::format("field '{}'", fname));
        }
        // Record the field's declared type on the field-name span so
        // that LSP hover can display it (the IdentifierNode is not a
        // Node*, so node_types can't be used).
        if (fi.type) {
          auto &st = current_instantiation_ ? current_instantiation_->span_types
                                            : span_types;
          st.push_back({node.fields[i].name.span, fi.type});
        }
        break;
      }
    }
    if (found)
      continue;

    // A key may also name an embed, which initialises the embedded value as a
    // whole — the only way to set storage a child field shadows.
    if (auto embed = embed_by_name(info, fname)) {
      if (!is_invalid_type(val_type))
        expect_assignable(node.span, embed, val_type,
                          std::format("embedded '{}'", fname));
      auto &st = current_instantiation_ ? current_instantiation_->span_types
                                        : span_types;
      st.push_back({node.fields[i].name.span, embed});
      continue;
    }

    error(node.span,
          std::format("struct '{}' has no field '{}'", info.name, fname));
  }

  // If the original type was an alias, return the alias type so the
  // variable retains its alias identity.
  if (type_expr_type->kind == TypeKind::Alias)
    return type_expr_type;

  return effective_type;
}

// ===========================================================================
// Struct operator overloading
// ===========================================================================

TypePtr Analyzer::check_struct_binary_expr(const BinaryExprNode &node,
                                            const Node &parent,
                                            const TypePtr &lhs,
                                            const TypePtr &rhs) {
  auto &info = std::get<StructTypeInfo>(lhs->detail);

  // Helper: returns true if the struct declares a method with the given name.
  auto has_method = [&](const std::string &name) -> bool {
    for (auto &m : info.methods)
      if (m.name == name)
        return true;
    return false;
  };

  // Helper: record the resolved method and return the result type.
  auto resolve = [&](const std::string &method, TypePtr result) -> TypePtr {
    if (current_instantiation_) {
      current_instantiation_->struct_operator_methods[&parent] = method;
    } else {
      struct_operator_methods[&parent] = method;
    }
    return result;
  };

  using K = Token::Kind;

  switch (node.op) {
  // ── Additive ───────────────────────────────────────────────────────────────
  case K::Add:
    if (has_method("Add")) {
      expect_assignable(node.rhs->span, lhs, rhs, "Add argument");
      return resolve("Add", lhs);
    }
    error(node.span,
          std::format("type {} does not implement Adder (no Add method)",
                      type_to_string(lhs)));
    return builtins.invalid_type;

  case K::Sub:
    if (has_method("Sub")) {
      expect_assignable(node.rhs->span, lhs, rhs, "Sub argument");
      return resolve("Sub", lhs);
    }
    error(node.span,
          std::format("type {} does not implement Subber (no Sub method)",
                      type_to_string(lhs)));
    return builtins.invalid_type;

  // ── Multiplicative ───────────────────────────────────────────────────────
  case K::Multiply:
    if (has_method("Mul")) {
      expect_assignable(node.rhs->span, lhs, rhs, "Mul argument");
      return resolve("Mul", lhs);
    }
    error(node.span,
          std::format("type {} does not implement Multiplier (no Mul method)",
                      type_to_string(lhs)));
    return builtins.invalid_type;

  case K::Divide:
    if (has_method("Div")) {
      expect_assignable(node.rhs->span, lhs, rhs, "Div argument");
      // Divisable returns T | Error (can fail, e.g. divide by zero).
      return resolve("Div", make_union_type({lhs, builtins.error_base}));
    }
    error(node.span,
          std::format("type {} does not implement Divisable (no Div method)",
                      type_to_string(lhs)));
    return builtins.invalid_type;

  // ── Equality ──────────────────────────────────────────────────────────────
  case K::Equal:
  case K::NotEqual:
    // Prefer Equals (runtime convention), then Equal (interface name),
    // then Compare as a fallback (Comparison.Equal == 1).
    if (has_method("Equals")) {
      expect_assignable(node.rhs->span, lhs, rhs, "Equals argument");
      return resolve("Equals", builtins.bool_type);
    }
    if (has_method("Equal")) {
      expect_assignable(node.rhs->span, lhs, rhs, "Equal argument");
      return resolve("Equal", builtins.bool_type);
    }
    if (has_method("Compare")) {
      // Fall back: Compare() == Comparison.Equal (1) → Bool.
      expect_assignable(node.rhs->span, lhs, rhs, "Compare argument");
      return resolve("Compare", builtins.bool_type);
    }
    error(node.span,
          std::format("type {} does not support equality (no Equals, Equal, "
                      "or Compare method)",
                      type_to_string(lhs)));
    return builtins.invalid_type;

  // ── Ordering ──────────────────────────────────────────────────────────────
  case K::LessThan:
  case K::LessThanEqual:
  case K::GreaterThan:
  case K::GreaterThanEqual:
    if (has_method("Compare")) {
      expect_assignable(node.rhs->span, lhs, rhs, "Compare argument");
      return resolve("Compare", builtins.bool_type);
    }
    error(node.span,
          std::format("type {} does not implement Comparable (no Compare "
                      "method)",
                      type_to_string(lhs)));
    return builtins.invalid_type;

  default:
    error(node.span,
          std::format("operator not supported for type {}",
                      type_to_string(lhs)));
    return builtins.invalid_type;
  }
}

TypePtr Analyzer::check_binary_expr(const BinaryExprNode &node,
                                    const Node &parent) {
  // A `.Variant` operand (an enum comparison, `c == .Red`) resolves against the
  // other operand's enum type; the other side is checked first to supply it.
  bool lhs_sh = std::get_if<EnumShorthandNode>(&node.lhs->data) != nullptr;
  bool rhs_sh = std::get_if<EnumShorthandNode>(&node.rhs->data) != nullptr;
  TypePtr lhs, rhs;
  if (rhs_sh && !lhs_sh) {
    lhs = check_expr(*node.lhs);
    rhs = check_expr_expecting(*node.rhs, lhs);
  } else if (lhs_sh && !rhs_sh) {
    rhs = check_expr(*node.rhs);
    lhs = check_expr_expecting(*node.lhs, rhs);
  } else {
    lhs = check_expr(*node.lhs);
    rhs = check_expr(*node.rhs);
  }

  if (is_invalid_type(lhs) || is_invalid_type(rhs))
    return builtins.invalid_type;

  // Errors compare by value: `==`/`!=` on two errors is structural (same type
  // and equal fields). Errors have no methods, so they never reach the struct
  // operator-overload path below. Structural comparison needs a concrete layout,
  // so at least one side must have a concrete error type — comparing two base
  // `error` values can't see their extra fields and is rejected.
  using EK = Token::Kind;
  if ((node.op == EK::Equal || node.op == EK::NotEqual) &&
      is_error_valued(lhs) && is_error_valued(rhs)) {
    if (is_abstract_error(lhs) && is_abstract_error(rhs))
      error(node.span,
            "cannot compare two base `error` values; at least one side must "
            "have a concrete error type (narrow with `is` first)");
    return builtins.bool_type;
  }

  // Enums and errors are identity types: they carry no arithmetic and cannot
  // overload it. Reject arithmetic here — errors would otherwise reach the
  // struct operator-overload path and be told they lack an `Add` method they
  // can never define (methods cannot be attached to error types).
  switch (node.op) {
  case EK::Add:
  case EK::Sub:
  case EK::Multiply:
  case EK::Divide:
  case EK::Pow:
  case EK::Modulo:
    for (auto &operand : {lhs, rhs}) {
      if (is_enum_valued(operand) || is_error_valued(operand)) {
        error(node.span,
              std::format("type {} does not support arithmetic operators",
                          type_to_string(operand)));
        return builtins.invalid_type;
      }
    }
    break;
  default:
    break;
  }

  // ── Struct operator overloading ──────────────────────────────────────────
  // Dispatch to method-based overloading before the built-in numeric/string
  // paths, so user types can override operators on structs.
  if (lhs->kind == TypeKind::Struct) {
    return check_struct_binary_expr(node, parent, lhs, rhs);
  }

  using K = Token::Kind;
  switch (node.op) {
  // Arithmetic: + - * ** %
  case K::Add:
  case K::Sub:
  case K::Multiply:
  case K::Pow:
  case K::Modulo: {
    // String concatenation with +.
    if (node.op == K::Add && lhs->kind == TypeKind::String &&
        rhs->kind == TypeKind::String) {
      return builtins.string_type;
    }
    if (!is_numeric(lhs)) {
      error(node.lhs->span,
            std::format("arithmetic operator requires numeric type, got {}",
                        type_to_string(lhs)));
      return builtins.invalid_type;
    }
    if (!is_numeric(rhs)) {
      error(node.rhs->span,
            std::format("arithmetic operator requires numeric type, got {}",
                        type_to_string(rhs)));
      return builtins.invalid_type;
    }
    return common_type(lhs, rhs);
  }

  // Division: returns T | Error (division by zero).
  case K::Divide: {
    if (!is_numeric(lhs) || !is_numeric(rhs)) {
      error(node.span,
            std::format("division requires numeric types, got {} and {}",
                        type_to_string(lhs), type_to_string(rhs)));
      return builtins.invalid_type;
    }
    auto result = common_type(lhs, rhs);
    return make_union_type({result, builtins.error_base});
  }

  // Comparison: == != > < >= <=
  case K::Equal:
  case K::NotEqual: {
    // Type tests are spelled `value is Type` (see check_is_expr), not `==`.
    if (!is_equatable(lhs)) {
      error(node.lhs->span, std::format("type {} does not support equality",
                                        type_to_string(lhs)));
      return builtins.invalid_type;
    }
    expect_assignable(node.rhs->span, lhs, rhs, "comparison");
    return builtins.bool_type;
  }
  case K::LessThan:
  case K::LessThanEqual:
  case K::GreaterThan:
  case K::GreaterThanEqual: {
    if (!is_ordered(lhs)) {
      error(node.lhs->span, std::format("type {} does not support ordering",
                                        type_to_string(lhs)));
      return builtins.invalid_type;
    }
    expect_assignable(node.rhs->span, lhs, rhs, "comparison");
    return builtins.bool_type;
  }

  // Logical: && ||
  case K::LogicalAnd:
  case K::LogicalOr: {
    expect_bool(node.lhs->span, lhs, "logical operator lhs");
    expect_bool(node.rhs->span, rhs, "logical operator rhs");
    return builtins.bool_type;
  }

  // Bitwise: & | ^ << >>
  case K::BitwiseAnd:
  case K::BitwiseOr:
  case K::BitwiseXor:
  case K::LeftShift:
  case K::RightShift: {
    if (lhs->kind != TypeKind::Int) {
      error(node.lhs->span,
            std::format("bitwise operator requires integer type, got {}",
                        type_to_string(lhs)));
      return builtins.invalid_type;
    }
    if (rhs->kind != TypeKind::Int) {
      error(node.rhs->span,
            std::format("bitwise operator requires integer type, got {}",
                        type_to_string(rhs)));
      return builtins.invalid_type;
    }
    return common_type(lhs, rhs);
  }

  default:
    error(node.span, "unsupported binary operator");
    return builtins.invalid_type;
  }
}

TypePtr Analyzer::check_unary_expr(const UnaryExprNode &node) {
  auto operand = check_expr(*node.operand);
  if (is_invalid_type(operand))
    return builtins.invalid_type;

  if (node.op == Token::Kind::Not) {
    expect_bool(node.operand->span, operand, "logical not");
    return builtins.bool_type;
  }
  if (node.op == Token::Kind::Sub) {
    if (is_enum_valued(operand) || is_error_valued(operand)) {
      error(node.operand->span,
            std::format("type {} does not support arithmetic operators",
                        type_to_string(operand)));
      return builtins.invalid_type;
    }
    if (!is_numeric(operand)) {
      error(node.operand->span,
            std::format("negation requires numeric type, got {}",
                        type_to_string(operand)));
      return builtins.invalid_type;
    }
    return operand;
  }
  if (node.op == Token::Kind::BitwiseNot) {
    if (operand->kind != TypeKind::Int) {
      error(node.operand->span,
            std::format("bitwise NOT requires integer type, got {}",
                        type_to_string(operand)));
      return builtins.invalid_type;
    }
    return operand;
  }

  error(node.span, "unsupported unary operator");
  return builtins.invalid_type;
}

TypePtr Analyzer::check_is_expr(const IsExpr &node) {
  auto value_type = check_expr(*node.value);
  auto test_type = resolve_type(*node.type);
  record_type(*node.type, test_type);

  if (is_invalid_type(value_type) || is_invalid_type(test_type))
    return builtins.bool_type;

  if (value_type->kind == TypeKind::Union) {
    auto &info = std::get<UnionTypeInfo>(value_type->detail);
    bool found = false;
    for (auto &alt : info.alternatives) {
      if (types_equal(alt, test_type) || is_assignable_to(test_type, alt)) {
        found = true;
        break;
      }
    }
    if (!found)
      error(node.type->span,
            std::format("type {} is not an alternative of {}",
                        type_to_string(test_type), type_to_string(value_type)));
  }

  return builtins.bool_type;
}

TypePtr Analyzer::check_group_expr(const GroupExprNode &node) {
  return check_expr(*node.inner);
}

static std::string callee_display_name(const Node &callee) {
  if (auto *id = std::get_if<IdentifierNode>(&callee.data))
    return std::string(id->name);
  if (auto *sel = std::get_if<SelectorNode>(&callee.data))
    return std::string(sel->field.name);
  return "function";
}

TypePtr Analyzer::check_call_expr(const CallExprNode &node,
                                  const Node &parent) {
  // Gate all intrinsic_* calls to stdlib packages only.
  if (auto *ident = std::get_if<IdentifierNode>(&node.callee->data)) {
    if (ident->name.starts_with("intrinsic_") && !is_stdlib) {
      error(node.callee->span,
            std::format("'{}' can only be called from stdlib packages",
                        ident->name));
      return builtins.invalid_type;
    }
  }

  auto callee_type = check_expr(*node.callee);
  if (is_invalid_type(callee_type))
    return builtins.invalid_type;

  if (!is_callable(callee_type)) {
    error(node.callee->span,
          std::format("'{}' is not callable", type_to_string(callee_type)));
    return builtins.invalid_type;
  }

  // A function-typed alias (`type Op = fn(...) ...`) is callable through its
  // underlying signature.
  callee_type = unwrap_alias(callee_type);

  // Check arguments first to collect their types.  A `.Variant` shorthand arg
  // resolves against the matching parameter's (concrete enum) type.
  const std::vector<TypePtr> *params =
      callee_type->kind == TypeKind::Func
          ? &std::get<FuncTypeInfo>(callee_type->detail).params
          : nullptr;
  std::vector<TypePtr> arg_types;
  for (size_t i = 0; i < node.args.size(); ++i) {
    TypePtr expected = (params && i < params->size()) ? (*params)[i] : nullptr;
    arg_types.push_back(check_expr_expecting(*node.args[i], expected));
  }

  // If the callee contains type parameters, attempt generic instantiation.
  auto effective_type = callee_type;
  if (has_type_params(callee_type)) {
    std::unordered_map<uint32_t, TypePtr> bindings;
    auto instantiated =
        instantiate_generic_call(callee_type, arg_types, node.span, &bindings);
    if (!is_invalid_type(instantiated))
      effective_type = instantiated;

    // For generic free functions, analyse the body with these concrete
    // bindings so member-access, operator-overloading and capture
    // tracking see concrete types.  Methods on generic types (Array/Map)
    // and receiver-method calls go through their own paths and aren't
    // registered in generic_templates_.
    if (!bindings.empty() && callee_type) {
      auto fd_it = func_decl_by_type_.find(callee_type.get());
      if (fd_it != func_decl_by_type_.end() &&
          !fd_it->second->is_extern &&
          generic_templates_.find(fd_it->second) != generic_templates_.end()) {
        // Substitution stops at a struct boundary, so a generic struct named
        // in the signature keeps its own type parameters however the call
        // binds them. Lowering that reads the value through the wrong layout,
        // so refuse the call rather than answer wrongly. Only a specialisable
        // function is checked: elsewhere a leftover parameter means a template
        // body being checked generically, where nothing is concrete yet.
        if (!is_invalid_type(effective_type) &&
            has_type_params(effective_type)) {
          error(node.span,
                std::format("cannot call '{}': its signature names a generic "
                            "struct that inference does not substitute",
                            callee_display_name(*node.callee)));
          return builtins.invalid_type;
        }
        instantiate_generic_body(*fd_it->second, bindings, parent);
        if (current_instantiation_) {
          current_instantiation_->node_type_args[&parent] = bindings;
        } else {
          node_type_args[&parent] = bindings;
        }
      }
    }
  } else if (auto *sel = std::get_if<SelectorNode>(&node.callee->data)) {
    // kind_methods_ call (Array/Map receiver) where the substituted
    // signature is already concrete, but the body must be re-checked
    // with concrete K/V bindings because it dispatches through a named
    // protocol on a TypeParam value.  Drive instantiation per concrete
    // K so codegen can specialise.
    auto obj_sem = node_types.count(sel->object.get())
                       ? node_types[sel->object.get()]
                       : nullptr;
    if (obj_sem && (obj_sem->kind == TypeKind::Array ||
                    obj_sem->kind == TypeKind::Map)) {
      // Cross-package: the FuncDecl and dispatch flag are missing until
      // we lazily load std/array or std/map source.  Trigger that here
      // so kind_method_decls_ / kind_method_uses_typeparam_dispatch_
      // are populated before we look them up.
      if (!is_stdlib && kind_method_decls_.find(obj_sem->kind) ==
                            kind_method_decls_.end()) {
        const char *origin = obj_sem->kind == TypeKind::Array
                                  ? "array" : "map";
        ensure_source_loaded(origin);
      }
      auto km_it = kind_method_decls_.find(obj_sem->kind);
      if (km_it != kind_method_decls_.end()) {
        auto m_it = km_it->second.find(std::string(sel->field.name));
        if (m_it != km_it->second.end() &&
            is_kind_method_mutating(*m_it->second.decl)) {
          if (auto *recv_id =
                  std::get_if<IdentifierNode>(&sel->object->data)) {
            auto sym = lookup(std::string(recv_id->name));
            if (sym && sym->kind == SymbolKind::Constant) {
              error(node.span,
                    std::format("cannot call mutating method '{}' on "
                                "constant '{}'",
                                sel->field.name, recv_id->name));
            }
          }
        }
        if (m_it != km_it->second.end() &&
            kind_method_uses_typeparam_dispatch_.count(m_it->second.decl)) {
          std::unordered_map<uint32_t, TypePtr> bindings;
          auto &tps = m_it->second.type_params;
          if (obj_sem->kind == TypeKind::Array && !tps.empty()) {
            auto &arr = std::get<ArrayTypeInfo>(obj_sem->detail);
            bindings[tps[0].id] = arr.element;
          } else if (obj_sem->kind == TypeKind::Map && tps.size() >= 2) {
            auto &mp = std::get<MapTypeInfo>(obj_sem->detail);
            bindings[tps[0].id] = mp.key;
            bindings[tps[1].id] = mp.value;
          }
          if (!bindings.empty()) {
            instantiate_generic_body(*m_it->second.decl, bindings, parent);
            if (current_instantiation_)
              current_instantiation_->node_type_args[&parent] = bindings;
            else
              node_type_args[&parent] = bindings;
          }
        }
      }

      // Array.String() / Map.String() require their element/key/value types
      // to satisfy Stringable (recursively for nested aggregates).  Phase 5
      // will migrate these out of builtins; the named-protocol diagnostic
      // here is the use-site enforcement promised by Phase 3.
      if (sel->field.name == "String" && node.args.empty()) {
        if (obj_sem->kind == TypeKind::Array) {
          auto &arr = std::get<ArrayTypeInfo>(obj_sem->detail);
          check_stringable_recursive(arr.element, sel->field.span,
                                     "array element of .String() receiver");
        } else if (obj_sem->kind == TypeKind::Map) {
          auto &mp = std::get<MapTypeInfo>(obj_sem->detail);
          check_stringable_recursive(mp.key, sel->field.span,
                                     "map key of .String() receiver");
          check_stringable_recursive(mp.value, sel->field.span,
                                     "map value of .String() receiver");
        }
      }
    }
  }

  auto &fn_info = std::get<FuncTypeInfo>(effective_type->detail);

  // Check argument count.
  if (!fn_info.is_variadic) {
    if (arg_types.size() != fn_info.params.size()) {
      error(node.span, std::format("expected {} argument(s), got {}",
                                   fn_info.params.size(), arg_types.size()));
      return builtins.invalid_type;
    }
  } else {
    if (fn_info.params.size() > 0 &&
        arg_types.size() < fn_info.params.size() - 1) {
      error(node.span,
            std::format("expected at least {} argument(s), got {}",
                        fn_info.params.size() - 1, arg_types.size()));
      return builtins.invalid_type;
    }
  }

  // Spec: arrays of the same element type may be passed directly into
  // a variadic without spreading.  (docs/language.md:276-285)
  // Detection: exactly one argument in the variadic position whose type
  // matches the variadic's array type.
  bool variadic_array_passthrough =
      fn_info.is_variadic && !fn_info.params.empty() &&
      arg_types.size() == fn_info.params.size() &&
      fn_info.params.back()->kind == TypeKind::Array &&
      arg_types.back() && arg_types.back()->kind == TypeKind::Array &&
      types_equal(arg_types.back(), fn_info.params.back());

  // Check argument types against the (possibly instantiated) signature.
  for (size_t i = 0; i < arg_types.size(); ++i) {
    bool is_variadic_param = fn_info.is_variadic && !fn_info.params.empty() &&
                             i >= fn_info.params.size() - 1;
    if (is_variadic_param && variadic_array_passthrough) {
      expect_assignable(node.args[i]->span, fn_info.params.back(),
                        arg_types[i],
                        std::format("variadic argument {}", i + 1));
    } else if (is_variadic_param) {
      // Variadic args are checked against the element type of the
      // array-wrapped last parameter.
      auto &last = fn_info.params.back();
      if (last->kind == TypeKind::Array) {
        auto &arr = std::get<ArrayTypeInfo>(last->detail);
        expect_assignable(node.args[i]->span, arr.element, arg_types[i],
                          std::format("variadic argument {}", i + 1));
      }
    } else if (i < fn_info.params.size()) {
      expect_assignable(node.args[i]->span, fn_info.params[i], arg_types[i],
                        std::format("argument {}", i + 1));
    }
  }

  // Return type.
  return fn_info.return_type ? fn_info.return_type : builtins.void_type;
}

TypePtr Analyzer::check_index_expr(const IndexExprNode &node) {
  auto obj_type = check_expr(*node.object);
  if (is_invalid_type(obj_type))
    return builtins.invalid_type;

  // Check for slice.
  if (std::holds_alternative<SliceNode>(node.index->data)) {
    auto &slice = std::get<SliceNode>(node.index->data);
    if (slice.low)
      check_expr(**slice.low);
    if (slice.high)
      check_expr(**slice.high);

    // Slicing a string or array returns the same type.
    if (obj_type->kind == TypeKind::String)
      return builtins.string_type;
    if (obj_type->kind == TypeKind::Array)
      return obj_type;

    error(node.span,
          std::format("cannot slice type {}", type_to_string(obj_type)));
    return builtins.invalid_type;
  }

  auto index_type = check_expr(*node.index);

  switch (obj_type->kind) {
  case TypeKind::Array: {
    auto &arr = std::get<ArrayTypeInfo>(obj_type->detail);
    if (!is_invalid_type(index_type) && index_type->kind != TypeKind::Int) {
      error(node.index->span, "array index must be an integer");
    }
    // Indexing returns T | Error (out of bounds).
    return make_union_type({arr.element, builtins.error_base});
  }
  case TypeKind::Map: {
    auto &map_info = std::get<MapTypeInfo>(obj_type->detail);
    if (!is_invalid_type(index_type)) {
      expect_assignable(node.index->span, map_info.key, index_type, "map key");
    }
    // Map access returns V | Error (missing key).
    return make_union_type({map_info.value, builtins.error_base});
  }
  case TypeKind::String: {
    if (!is_invalid_type(index_type) && index_type->kind != TypeKind::Int) {
      error(node.index->span, "string index must be an integer");
    }
    return builtins.string_type;
  }
  default:
    error(node.span, std::format("type {} does not support indexing",
                                 type_to_string(obj_type)));
    return builtins.invalid_type;
  }
}

TypePtr Analyzer::resolve_module_selector(const ModuleTypeInfo &mod,
                                          const std::string &field_name,
                                          Span field_span) {
  for (auto &exp : mod.exports)
    if (exp.name == field_name)
      return exp.type ? exp.type
                      : poison(field_span, "package export '" + field_name +
                                               "' has no type");
  error(field_span,
        std::format("package '{}' has no exported member '{}'", mod.name,
                    field_name));
  return builtins.invalid_type;
}

namespace {

// Returns the (substituted) member signature for an Array/Map kind method.
TypePtr substitute_kind_method(TypeKind kind, const TypePtr &effective_type,
                               const TypePtr &sig) {
  if (kind == TypeKind::Map) {
    auto &map_info = std::get<MapTypeInfo>(effective_type->detail);
    std::unordered_map<uint32_t, TypePtr> bindings;
    bindings[9991] = map_info.key;
    bindings[9992] = map_info.value;
    return substitute(sig, bindings);
  }
  if (kind == TypeKind::Array) {
    auto &arr_info = std::get<ArrayTypeInfo>(effective_type->detail);
    std::unordered_map<uint32_t, TypePtr> bindings;
    bindings[9990] = arr_info.element;
    return substitute(sig, bindings);
  }
  return sig;
}

} // namespace

TypePtr Analyzer::resolve_struct_member(const TypePtr &owner_type,
                                        const std::string &field_name,
                                        Span field_span) {
  auto &info = std::get<StructTypeInfo>(owner_type->detail);
  for (auto &f : info.fields)
    if (f.name == field_name)
      return f.type ? f.type
                    : poison(field_span,
                             "struct field '" + field_name + "' has no type");

  for (auto &m : info.methods) {
    if (m.name != field_name)
      continue;
    auto sig = m.signature ? m.signature : builtins.invalid_type;
    // A method is monomorphised from the receiver's type arguments alone, so
    // on a concrete instantiation nothing generic may be left in its
    // signature — neither a method-own type parameter nor a generic struct
    // the receiver's arguments do not reach.
    if (!info.type_args.empty() && has_type_params(sig)) {
      error(field_span,
            std::format("cannot call '{}' on '{}': its signature has type "
                        "parameters that the receiver does not bind",
                        field_name, type_to_string(owner_type)));
      return builtins.invalid_type;
    }
    return sig;
  }

  // An embed answers to its own type name, which reaches the embedded value
  // itself. This is the only way to read storage a child field shadows, so it
  // is checked before the promoted-member recursion.
  if (auto embed = embed_by_name(info, field_name))
    return embed;

  // Promoted members: recurse through embeds so a member declared several
  // levels deep is still found. Own members are checked above first, so a
  // child member shadows an embedded one of the same name.
  for (auto &embed : info.embeds) {
    if (!embed || embed->kind != TypeKind::Struct)
      continue;
    if (auto t = resolve_struct_member(embed, field_name, field_span))
      return t;
  }
  return nullptr;
}

// An embed answers to its bare type name, so anything else claiming that name
// would leave the embedded value unreachable. Two embeds of the same name from
// different packages collide for the same reason.
bool Analyzer::embed_name_taken(const Node &embed_node, const TypePtr &embed,
                                const std::vector<FieldInfo> &fields,
                                const std::vector<TypePtr> &seen) {
  auto &einfo = std::get<StructTypeInfo>(embed->detail);

  for (auto &f : fields) {
    if (f.name != einfo.name)
      continue;
    error(embed_node.span,
          std::format("embedded type '{}' collides with a field of the same "
                      "name",
                      einfo.name));
    return true;
  }

  for (auto &other : seen) {
    if (std::get<StructTypeInfo>(other->detail).name != einfo.name)
      continue;
    error(embed_node.span,
          std::format("embedded type '{}' collides with another embed of the "
                      "same name",
                      einfo.name));
    return true;
  }

  return false;
}

TypePtr Analyzer::embed_by_name(const StructTypeInfo &info,
                                const std::string &name) {
  for (auto &embed : info.embeds) {
    if (!embed || embed->kind != TypeKind::Struct)
      continue;
    if (std::get<StructTypeInfo>(embed->detail).name == name)
      return embed;
  }
  return nullptr;
}

void Analyzer::collect_promoted_fields(const StructTypeInfo &info,
                                       std::vector<FieldInfo> &out) {
  out.insert(out.end(), info.fields.begin(), info.fields.end());
  for (auto &embed : info.embeds) {
    if (embed && embed->kind == TypeKind::Struct)
      collect_promoted_fields(std::get<StructTypeInfo>(embed->detail), out);
  }
}

TypePtr Analyzer::resolve_method_signature(const TypePtr &obj_type,
                                           const std::string &field_name,
                                           Span span) {
  auto canonicalize_intrinsic = [this](const TypePtr &t) -> const Type * {
    switch (t->kind) {
    case TypeKind::Int: {
      auto &ii = std::get<IntType>(t->detail);
      if (ii.bits == 0) return builtins.int_type.get();
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
    case TypeKind::Float: {
      auto &fi = std::get<FloatType>(t->detail);
      if (fi.bits == 0) return builtins.float_type.get();
      if (fi.bits == 32) return builtins.float32_type.get();
      if (fi.bits == 64) return builtins.float64_type.get();
      return nullptr;
    }
    case TypeKind::Bool:   return builtins.bool_type.get();
    case TypeKind::String: return builtins.string_type.get();
    default: return nullptr;
    }
  };

  auto find_user_methods = [&]() -> const std::vector<MethodInfo> * {
    auto it = type_methods_.find(obj_type.get());
    if (it == type_methods_.end() && obj_type->kind == TypeKind::Alias) {
      auto underlying = unwrap_alias(obj_type);
      if (underlying)
        it = type_methods_.find(underlying.get());
    }
    if (it == type_methods_.end()) {
      const Type *canonical = canonicalize_intrinsic(obj_type);
      if (canonical && canonical != obj_type.get())
        it = type_methods_.find(canonical);
    }
    return it == type_methods_.end() ? nullptr : &it->second;
  };

  if (auto *vec = find_user_methods())
    for (auto &m : *vec)
      if (m.name == field_name)
        return m.signature ? m.signature
                           : poison(span, "method '" + field_name +
                                              "' has no signature");

  auto effective_kind = underlying_kind(obj_type);
  auto effective_type = unwrap_alias(obj_type);

  auto kind_it = kind_methods_.find(effective_kind);
  if (kind_it != kind_methods_.end()) {
    for (auto &m : kind_it->second) {
      if (m.name != field_name)
        continue;
      if (!m.signature)
        return poison(span,
                      "kind method '" + field_name + "' has no signature");
      if (has_type_params(m.signature))
        return substitute_kind_method(effective_kind, effective_type,
                                      m.signature);
      return m.signature;
    }
  }

  for (auto &m : builtin_methods(effective_kind, builtins)) {
    if (m.name != field_name)
      continue;
    if (!m.signature)
      return poison(span,
                    "builtin method '" + field_name + "' has no signature");
    if (has_type_params(m.signature))
      return substitute_kind_method(effective_kind, effective_type,
                                    m.signature);
    return m.signature;
  }

  // Named-protocol fallback for TypeParam values inside generic bodies.
  // A method call on a value of TypeParam type is accepted if the called
  // method belongs to a compiler-known protocol (Hashable, Stringable);
  // satisfaction by the concrete type is verified at the monomorphisation
  // site.  Self-typed positions in the protocol signature are rewritten to
  // refer to the TypeParam.
  if (obj_type->kind == TypeKind::TypeParam) {
    auto subst_self = [](const TypePtr &sig, const TypePtr &iface,
                          const TypePtr &concrete) -> TypePtr {
      if (!sig || sig->kind != TypeKind::Func) return sig;
      auto &fi = std::get<FuncTypeInfo>(sig->detail);
      auto &iinfo = std::get<InterfaceTypeInfo>(iface->detail);
      auto subst_one = [&](const TypePtr &t) -> TypePtr {
        if (t && t->kind == TypeKind::Interface) {
          auto &ti = std::get<InterfaceTypeInfo>(t->detail);
          if (ti.name == iinfo.name &&
              ti.origin_package == iinfo.origin_package)
            return concrete;
        }
        return t;
      };
      std::vector<TypePtr> nparams;
      for (auto &p : fi.params)  nparams.push_back(subst_one(p));
      TypePtr nret = fi.return_type ? subst_one(fi.return_type) : nullptr;
      auto out = make_func_type(std::move(nparams), std::move(nret));
      std::get<FuncTypeInfo>(out->detail).is_variadic = fi.is_variadic;
      return out;
    };
    auto try_protocol = [&](const TypePtr &iface) -> TypePtr {
      if (!iface || iface->kind != TypeKind::Interface) return nullptr;
      auto &ii = std::get<InterfaceTypeInfo>(iface->detail);
      for (auto &m : ii.methods)
        if (m.name == field_name && m.signature)
          return subst_self(m.signature, iface, obj_type);
      return nullptr;
    };
    auto record_dispatch = [&]() {
      if (current_eager_kind_method_decl_)
        kind_method_uses_typeparam_dispatch_.insert(
            current_eager_kind_method_decl_);
    };
    auto &tpinfo = std::get<TypeParamInfo>(obj_type->detail);
    if (tpinfo.bound) {
      if (auto t = try_protocol(*tpinfo.bound)) {
        record_dispatch();
        return t;
      }
      return nullptr;
    }
    if (auto t = try_protocol(builtins.stringable_iface)) {
      record_dispatch();
      return t;
    }
    if (auto t = try_protocol(builtins.hashable_iface)) {
      record_dispatch();
      return t;
    }
  }

  if (auto u = unwrap_structural_alias(obj_type);
      u && u->kind == TypeKind::Union)
    return resolve_union_method(u, field_name);

  return nullptr;
}

TypePtr Analyzer::resolve_union_method(const TypePtr &union_type,
                                       const std::string &field_name) {
  auto &alts = std::get<UnionTypeInfo>(union_type->detail).alternatives;
  if (alts.empty())
    return nullptr;

  // Codegen dispatches per member through a struct- or scalar-receiver call;
  // other member kinds (array/map/alias/enum) have no receiver-call lowering
  // yet, so restrict eligibility to avoid accepting a call codegen can't emit.
  auto dispatchable = [](const TypePtr &t) {
    switch (t->kind) {
    case TypeKind::Struct:
    case TypeKind::Int:
    case TypeKind::Float:
    case TypeKind::Bool:
    case TypeKind::String:
      return true;
    default:
      return false;
    }
  };
  for (auto &alt : alts)
    if (!dispatchable(alt))
      return nullptr;

  auto self_free = [](const TypePtr &sig, const TypePtr &iface) -> bool {
    if (!sig || sig->kind != TypeKind::Func)
      return false;
    auto &fi = std::get<FuncTypeInfo>(sig->detail);
    auto &ii = std::get<InterfaceTypeInfo>(iface->detail);
    auto refs_self = [&](const TypePtr &t) {
      if (!t || t->kind != TypeKind::Interface)
        return false;
      auto &ti = std::get<InterfaceTypeInfo>(t->detail);
      return ti.name == ii.name && ti.origin_package == ii.origin_package;
    };
    for (auto &p : fi.params)
      if (refs_self(p))
        return false;
    return !(fi.return_type && refs_self(fi.return_type));
  };

  auto try_iface = [&](const TypePtr &iface) -> TypePtr {
    if (!iface || iface->kind != TypeKind::Interface)
      return nullptr;
    auto &ii = std::get<InterfaceTypeInfo>(iface->detail);
    const MethodInfo *m = nullptr;
    for (auto &im : ii.methods)
      if (im.name == field_name) {
        m = &im;
        break;
      }
    if (!m || !self_free(m->signature, iface))
      return nullptr;
    for (auto &alt : alts)
      if (!satisfies_interface(alt, iface))
        return nullptr;
    return m->signature;
  };

  if (auto s = try_iface(builtins.stringable_iface))
    return s;
  if (auto s = try_iface(builtins.hashable_iface))
    return s;
  if (package_scope_)
    for (auto &[name, sym] : package_scope_->symbols)
      if (sym.kind == SymbolKind::Type && sym.type &&
          sym.type->kind == TypeKind::Interface)
        if (auto s = try_iface(sym.type))
          return s;
  return nullptr;
}

TypePtr Analyzer::check_selector(const SelectorNode &node,
                                 const Node & /*parent*/) {
  auto obj_type = check_type_or_value_expr(*node.object);
  if (is_invalid_type(obj_type))
    return builtins.invalid_type;

  std::string field_name(node.field.name);

  // A type-name object accesses the type's own members, not an instance's.
  // For a struct that means its `fn Type.Fn()` type methods; a struct has no
  // instance fields or receiver methods reachable through the bare type name.
  if (auto *id = std::get_if<IdentifierNode>(&node.object->data)) {
    auto sym = lookup(std::string(id->name));
    if (sym && sym->kind == SymbolKind::Type &&
        obj_type->kind == TypeKind::Struct) {
      if (auto sig = lookup_struct_type_method(obj_type, field_name))
        return sig;
      error(node.field.span,
            std::format("type {} has no type method '{}'",
                        type_to_string(obj_type), field_name));
      return builtins.invalid_type;
    }
  }

  if (obj_type->kind == TypeKind::Module) {
    auto &mod = std::get<ModuleTypeInfo>(obj_type->detail);
    return resolve_module_selector(mod, field_name, node.field.span);
  }

  if (obj_type->kind == TypeKind::Struct)
    if (auto t = resolve_struct_member(obj_type, field_name, node.field.span))
      return t;

  if (obj_type->kind == TypeKind::Interface) {
    auto &info = std::get<InterfaceTypeInfo>(obj_type->detail);
    for (auto &m : info.methods)
      if (m.name == field_name)
        return m.signature ? m.signature
                           : poison(node.field.span,
                                    "interface method '" + field_name +
                                        "' has no signature");
  }

  if (obj_type->kind == TypeKind::Enum) {
    auto &info = std::get<EnumTypeInfo>(obj_type->detail);
    for (auto &v : info.variants)
      if (v.name == field_name)
        return obj_type;
    // `Enum.From(value)` reverse-lookup constructor: takes the backing value
    // (string for a string-backed enum, int otherwise) and returns the enum
    // or `Missing` when no variant matches.
    if (field_name == "From") {
      auto backing =
          info.string_backed ? builtins.string_type : builtins.int_type;
      return make_func_type(
          {backing}, {make_union_type({obj_type, builtins.error_base})});
    }
  }

  if (obj_type->kind == TypeKind::Alias) {
    auto &alias_info = std::get<AliasTypeInfo>(obj_type->detail);
    for (auto &m : alias_info.methods)
      if (m.name == field_name)
        return m.signature ? m.signature
                           : poison(node.field.span,
                                    "alias method '" + field_name +
                                        "' has no signature");
    auto underlying = unwrap_alias(obj_type);
    if (underlying && underlying->kind == TypeKind::Struct)
      if (auto t =
              resolve_struct_member(underlying, field_name, node.field.span))
        return t;
  }

  if (auto sig = resolve_method_signature(obj_type, field_name,
                                          node.field.span))
    return sig;

  error(node.field.span, std::format("type {} has no member '{}'",
                                     type_to_string(obj_type), field_name));
  return builtins.invalid_type;
}

TypePtr Analyzer::check_if_expr(const IfExprNode &node) {
  auto cond_type = check_expr(*node.condition);
  expect_bool(node.condition->span, cond_type);

  // Detect type-test pattern: `if value is Type` to narrow the variable in the
  // then-block (to Type) and the else-block (to the union minus Type).
  std::string narrowed_var;
  TypePtr narrowed_type = nullptr;
  TypePtr else_narrowed_type = nullptr;

  if (auto *is_expr = std::get_if<IsExpr>(&node.condition->data)) {
    if (auto *val_id = std::get_if<IdentifierNode>(&is_expr->value->data)) {
      auto lhs_sym = lookup(std::string(val_id->name));
      auto matched = resolve_type(*is_expr->type);
      if (lhs_sym && lhs_sym->type &&
          lhs_sym->type->kind == TypeKind::Union && matched &&
          !is_invalid_type(matched)) {
        narrowed_var = std::string(val_id->name);
        narrowed_type = matched;
        // Compute the else narrowed type (union minus matched type).
        auto &info = std::get<UnionTypeInfo>(lhs_sym->type->detail);
        std::vector<TypePtr> remaining;
        for (auto &alt : info.alternatives) {
          if (!types_equal(alt, matched))
            remaining.push_back(alt);
        }
        if (remaining.size() == 1)
          else_narrowed_type = remaining[0];
        else if (remaining.size() > 1)
          else_narrowed_type = make_union_type(std::move(remaining));
      }
    }
  }

  push_scope(ScopeKind::Block);
  if (!narrowed_var.empty() && narrowed_type) {
    // Narrow the variable in the then-scope.
    current_scope->symbols[narrowed_var] =
        Symbol::variable(narrowed_var, narrowed_type, node.then_block->span);
  }
  auto &then_block = std::get<BlockNode>(node.then_block->data);
  auto then_type = check_block(then_block);
  pop_scope();

  if (node.else_block) {
    push_scope(ScopeKind::Block);
    if (!narrowed_var.empty() && else_narrowed_type) {
      current_scope->symbols[narrowed_var] = Symbol::variable(
          narrowed_var, else_narrowed_type, (*node.else_block)->span);
    }
    auto &else_block = std::get<BlockNode>((*node.else_block)->data);
    auto else_type = check_block(else_block);
    pop_scope();
    return common_type(then_type, else_type);
  }

  return then_type;
}

TypePtr Analyzer::check_switch_expr(const SwitchExprNode &node) {
  auto subject_type = check_expr(*node.subject);
  TypePtr result_type = nullptr;

  // Detect if this is a type-matching switch (subject is a union and
  // patterns are type names).
  bool is_type_match = false;
  std::string subject_var;
  if (subject_type && subject_type->kind == TypeKind::Union) {
    if (auto *id = std::get_if<IdentifierNode>(&node.subject->data)) {
      subject_var = std::string(id->name);
    }
    // Check if the first arm's first pattern is a type name.
    if (!node.arms.empty() && !node.arms[0].patterns.empty()) {
      if (auto *pid =
              std::get_if<IdentifierNode>(&node.arms[0].patterns[0]->data)) {
        auto sym = lookup(std::string(pid->name));
        if (sym && sym->kind == SymbolKind::Type)
          is_type_match = true;
      }
    }
  }

  for (auto &arm : node.arms) {
    TypePtr first_pattern_type;
    for (auto &pat : arm.patterns) {
      TypePtr pattern_type;
      if (auto *sh = std::get_if<EnumShorthandNode>(&pat->data))
        pattern_type = check_enum_shorthand(*sh, *pat, subject_type);
      else
        pattern_type = check_type_or_value_expr(*pat);
      if (!first_pattern_type)
        first_pattern_type = pattern_type;

      if (is_type_match) {
        // Type matching: verify the pattern type is an alternative of the union.
        if (!is_invalid_type(pattern_type) && !is_invalid_type(subject_type)) {
          auto &info = std::get<UnionTypeInfo>(subject_type->detail);
          bool found = false;
          for (auto &alt : info.alternatives) {
            if (types_equal(alt, pattern_type) ||
                is_assignable_to(pattern_type, alt))
              found = true;
          }
          if (!found) {
            error(pat->span,
                  std::format("type {} is not an alternative of {}",
                              type_to_string(pattern_type),
                              type_to_string(subject_type)));
          }
        }
      } else {
        // Value matching: pattern must be same type as subject.
        if (!is_invalid_type(pattern_type) && !is_invalid_type(subject_type)) {
          expect_assignable(pat->span, subject_type, pattern_type,
                            "case pattern");
        }
      }
    }

    // Narrow the subject inside the arm body when there's a single
    // pattern; with multiple patterns the narrowed type would be their
    // union, which the analyzer doesn't synthesize here.  Narrowing
    // applies whether the body is a block or an expression — without
    // this, `case Int: x.String()` saw x as the unnarrowed union.
    bool narrowed = is_type_match && !subject_var.empty() &&
                    arm.patterns.size() == 1 &&
                    !is_invalid_type(first_pattern_type);
    push_scope(ScopeKind::Block);
    if (narrowed) {
      current_scope->symbols[subject_var] = Symbol::variable(
          subject_var, first_pattern_type, arm.patterns[0]->span);
    }
    TypePtr arm_type;
    if (auto *block = std::get_if<BlockNode>(&arm.body->data)) {
      arm_type = check_block(*block);
    } else {
      arm_type = check_expr(*arm.body);
    }
    pop_scope();

    if (!result_type)
      result_type = arm_type;
    else
      result_type = common_type(result_type, arm_type);
  }

  if (node.else_body) {
    TypePtr else_type;
    if (auto *block = std::get_if<BlockNode>(&(*node.else_body)->data)) {
      push_scope(ScopeKind::Block);
      else_type = check_block(*block);
      pop_scope();
    } else {
      else_type = check_expr(**node.else_body);
    }
    if (!result_type)
      result_type = else_type;
    else
      result_type = common_type(result_type, else_type);
  } else if (is_type_match && !is_invalid_type(subject_type)) {
    // Spec: type-matching without `else` must be exhaustive.
    // (docs/language.md:1174-1177)
    auto &info = std::get<UnionTypeInfo>(subject_type->detail);
    std::vector<TypePtr> uncovered;
    for (auto &alt : info.alternatives) {
      bool covered = false;
      for (auto &arm : node.arms) {
        for (auto &pat : arm.patterns) {
          auto pat_t = check_type_or_value_expr(*pat);
          if (is_invalid_type(pat_t)) continue;
          if (types_equal(alt, pat_t) || is_assignable_to(pat_t, alt)) {
            covered = true;
            break;
          }
        }
        if (covered) break;
      }
      if (!covered) uncovered.push_back(alt);
    }
    if (!uncovered.empty()) {
      std::string names;
      for (size_t i = 0; i < uncovered.size(); ++i) {
        if (i) names += ", ";
        names += type_to_string(uncovered[i]);
      }
      error(node.span,
            std::format("non-exhaustive type-switch: missing case(s) for {}",
                        names));
    }
  }

  return result_type ? result_type : builtins.void_type;
}

TypePtr Analyzer::check_for_expr(const ForExprNode &node,
                                 TypePtr accumulator_hint) {
  push_scope(ScopeKind::Loop);
  break_value_types_.emplace_back();

  if (node.mode) {
    std::visit(overloaded{
                   [&](const ForRangeClauseNode &range) {
                     auto iter_type = check_expr(*range.iterable);
                     // Infer loop variable types from the iterable.
                     TypePtr elem_type = builtins.invalid_type;
                     TypePtr key_type = builtins.int_type;

                     if (!is_invalid_type(iter_type)) {
                       switch (iter_type->kind) {
                       case TypeKind::Array: {
                         auto &arr = std::get<ArrayTypeInfo>(iter_type->detail);
                         elem_type = arr.element;
                         break;
                       }
                       case TypeKind::Map: {
                         auto &m = std::get<MapTypeInfo>(iter_type->detail);
                         key_type = m.key;
                         elem_type = m.value;
                         break;
                       }
                       case TypeKind::String:
                         elem_type = builtins.string_type;
                         break;
                       case TypeKind::Struct: {
                         // Task<T> is iterable — yields T from its channel.
                         auto &si = std::get<StructTypeInfo>(iter_type->detail);
                         if (si.name == "Task" && !si.type_params.empty()) {
                           // If T has been substituted, use the concrete
                           // element type; otherwise fall back to the param.
                           auto tp_id = si.type_params[0].id;
                           // Look for a concrete binding in fields/methods;
                           // for an instantiated Task the type_params list
                           // still contains the original TypeParam, but the
                           // methods have been substituted.  The Wait()
                           // method returns T|Error — grab T from there.
                           bool found = false;
                           for (auto &m : si.methods) {
                             if (m.name == "Wait" && m.signature &&
                                 m.signature->kind == TypeKind::Func) {
                               auto &fi = std::get<FuncTypeInfo>(
                                   m.signature->detail);
                               if (fi.return_type) {
                                 auto &ret = fi.return_type;
                                 if (ret->kind == TypeKind::Union) {
                                   auto &ui = std::get<UnionTypeInfo>(
                                       ret->detail);
                                   for (auto &alt : ui.alternatives) {
                                     if (alt->kind != TypeKind::Interface) {
                                       elem_type = alt;
                                       found = true;
                                       break;
                                     }
                                   }
                                 } else {
                                   elem_type = ret;
                                   found = true;
                                 }
                               }
                               break;
                             }
                           }
                           if (!found)
                             elem_type = builtins.invalid_type;
                         } else {
                           // Not a Task — check for the Iterable protocol:
                           // a Next() method returning T | Error.
                           bool found_iterable = false;
                           for (auto &m : si.methods) {
                             if (m.name != "Next" || !m.signature ||
                                 m.signature->kind != TypeKind::Func)
                               continue;
                             auto &fi =
                                 std::get<FuncTypeInfo>(m.signature->detail);
                             // Next() takes no explicit params (just self).
                             if (!fi.params.empty())
                               break;
                             if (!fi.return_type ||
                                 fi.return_type->kind != TypeKind::Union)
                               break;
                             // Extract T from T | Error.
                             auto &ui = std::get<UnionTypeInfo>(
                                 fi.return_type->detail);
                             for (auto &alt : ui.alternatives) {
                               if (alt->kind == TypeKind::Interface)
                                 continue; // skip Error
                               elem_type = alt;
                               found_iterable = true;
                               // Record for codegen so it knows which
                               // element type to extract.
                               if (current_instantiation_) {
                                 current_instantiation_
                                     ->iterable_next_elem_type[range.iterable
                                                                   .get()] =
                                     elem_type;
                               } else {
                                 iterable_next_elem_type[range.iterable.get()] =
                                     elem_type;
                               }
                               break;
                             }
                             break; // found Next(), stop method search
                           }
                           if (!found_iterable) {
                             error(
                                 range.iterable->span,
                                 std::format(
                                     "type {} is not iterable (no "
                                     "Next() T | Error method)",
                                     type_to_string(iter_type)));
                           }
                         }
                         break;
                       }
                       default:
                         error(range.iterable->span,
                               std::format("type {} is not iterable",
                                           type_to_string(iter_type)));
                         break;
                       }
                     }

                     if (range.vars.size() == 1) {
                       current_scope->symbols.emplace(
                           std::string(range.vars[0].name),
                           Symbol::variable(std::string(range.vars[0].name),
                                            elem_type, range.vars[0].span));
                     } else if (range.vars.size() == 2) {
                       current_scope->symbols.emplace(
                           std::string(range.vars[0].name),
                           Symbol::variable(std::string(range.vars[0].name),
                                            key_type, range.vars[0].span));
                       current_scope->symbols.emplace(
                           std::string(range.vars[1].name),
                           Symbol::variable(std::string(range.vars[1].name),
                                            elem_type, range.vars[1].span));
                     }
                   },
                   [&](const ForIterClauseNode &iter) {
                     check_stmt(*iter.init);
                     auto cond = check_expr(*iter.condition);
                     expect_bool(iter.condition->span, cond, "for condition");
                     check_stmt(*iter.update);
                   },
                   [&](const auto &) {
                     // Bare condition expression.
                     auto cond = check_expr(**node.mode);
                     expect_bool((*node.mode)->span, cond, "for condition");
                   },
               },
               (*node.mode)->data);
  }

  // Accumulator pipe — typed from the variable declaration's type hint.
  TypePtr acc_type = accumulator_hint ? accumulator_hint : builtins.void_type;
  if (node.accumulator) {
    current_scope->symbols.emplace(
        std::string(node.accumulator->name),
        Symbol::variable(std::string(node.accumulator->name), acc_type,
                         node.accumulator->span));
  }

  auto &body_block = std::get<BlockNode>(node.body->data);
  check_block(body_block);

  // A `break <value>` anywhere in the body shapes the for-expression's
  // result as `T | Error` (spec language.md:1284-1295).  When multiple
  // breaks have differing types the result is `T1 | T2 | … | Error`.
  std::vector<TypePtr> break_types = std::move(break_value_types_.back());
  break_value_types_.pop_back();

  pop_scope();

  if (!break_types.empty()) {
    std::vector<TypePtr> alts;
    for (auto &bt : break_types) {
      bool seen = false;
      for (auto &existing : alts)
        if (types_equal(existing, bt)) { seen = true; break; }
      if (!seen) alts.push_back(bt);
    }
    alts.push_back(builtins.error_base);
    return make_union_type(std::move(alts));
  }

  return node.accumulator ? acc_type : builtins.void_type;
}

TypePtr Analyzer::check_spawn_expr(const SpawnExprNode &node,
                                   const Node &parent) {
  push_scope(ScopeKind::Spawn);

  if (node.pipe) {
    current_scope->symbols.emplace(
        std::string(node.pipe->name),
        Symbol::variable(std::string(node.pipe->name), builtins.context_type,
                         node.pipe->span));
  }

  if (auto *block = std::get_if<BlockNode>(&node.body->data)) {
    check_block(*block);
  } else {
    check_expr(*node.body);
  }

  // Update spawn capture types now that type-checking is complete.
  auto &caps_map =
      current_instantiation_ ? current_instantiation_->spawn_captures
                             : spawn_captures;
  auto cap_it = caps_map.find(&parent);
  if (cap_it != caps_map.end()) {
    for (auto &cap : cap_it->second) {
      auto sym = current_scope->lookup(cap.name);
      if (!sym) {
        // Try the parent scope (the capture is from outside spawn).
        auto outer = current_scope->parent;
        while (outer) {
          sym = outer->lookup_local(cap.name);
          if (sym)
            break;
          outer = outer->parent;
        }
      }
      if (sym && sym->type) {
        cap.type = sym->type;
        // Re-classify with the resolved type.
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
  }

  pop_scope();

  // Default Task instantiation: spawn-with-no-explicit-T produces Task<Void>.
  // Codegen needs a concrete T to lower task.Wait() (the union it produces is
  // T | Error); without one, the Wait call drops out of the IR and the
  // parent prints before the spawn body runs.
  TypePtr chan_type = builtins.void_type;
  if (node.generic && !node.generic->type_params.empty()) {
    auto explicit_t = resolve_type(*node.generic->type_params[0]);
    if (explicit_t && !is_invalid_type(explicit_t))
      chan_type = explicit_t;
  }
  return instantiate_task_type(chan_type);
}

TypePtr Analyzer::instantiate_task_type(const TypePtr &chan_type) {
  std::unordered_map<uint32_t, TypePtr> bindings;
  bindings[0] = chan_type; // T (id 0) in Task<T>

  auto &info = std::get<StructTypeInfo>(builtins.task_type->detail);
  std::vector<FieldInfo> new_fields;
  for (auto &f : info.fields)
    new_fields.push_back({f.name, substitute(f.type, bindings), f.is_public});
  std::vector<MethodInfo> new_methods;
  for (auto &m : info.methods)
    new_methods.push_back({m.name, substitute(m.signature, bindings),
                           m.is_public, m.origin_package});

  auto result = make_struct_type(info.name, std::move(new_fields),
                                 std::move(new_methods), {},
                                 info.origin_package);
  auto &ri = std::get<StructTypeInfo>(result->detail);
  ri.type_params = info.type_params;
  ri.type_args.push_back(chan_type);
  ri.embeds = info.embeds;
  return result;
}

// The type bound to an `or |err|` pipe: the union's error alternative(s). A base
// `error` slot (or a mix that includes it) widens to base `error`; otherwise the
// concrete error type, or a union of the concrete errors.
TypePtr Analyzer::or_error_type(const TypePtr &union_type) {
  if (!union_type || union_type->kind != TypeKind::Union)
    return builtins.error_base;
  auto &info = std::get<UnionTypeInfo>(union_type->detail);
  std::vector<TypePtr> errs;
  for (auto &alt : info.alternatives) {
    if (is_abstract_error(alt))
      return builtins.error_base;
    if (is_error_valued(alt))
      errs.push_back(alt);
  }
  if (errs.size() == 1)
    return errs[0];
  if (errs.empty())
    return builtins.error_base;
  return make_union_type(std::move(errs));
}

TypePtr Analyzer::check_or_expr(const OrExprNode &node) {
  auto expr_type = check_expr(*node.expr);

  if (is_invalid_type(expr_type)) {
    // Still check the fallback block for internal errors.
    push_scope(ScopeKind::Block);
    if (node.pipe) {
      current_scope->symbols.emplace(
          std::string(node.pipe->name),
          Symbol::variable(std::string(node.pipe->name), builtins.error_base,
                           node.pipe->span));
    }
    auto &block = std::get<BlockNode>(node.fallback->data);
    check_block(block);
    pop_scope();
    return builtins.invalid_type;
  }

  // The or-clause strips the error from the union.
  push_scope(ScopeKind::Block);

  if (node.pipe) {
    current_scope->symbols.emplace(
        std::string(node.pipe->name),
        Symbol::variable(std::string(node.pipe->name),
                         or_error_type(expr_type), node.pipe->span));
  }

  auto &block = std::get<BlockNode>(node.fallback->data);
  auto fallback_type = check_block(block);

  pop_scope();

  // Strip error members from the union to get the purified type.
  if (expr_type->kind == TypeKind::Union) {
    auto &info = std::get<UnionTypeInfo>(expr_type->detail);
    std::vector<TypePtr> purified;
    for (auto &alt : info.alternatives) {
      if (!is_error_valued(alt))
        purified.push_back(alt);
    }
    if (purified.empty())
      return fallback_type ? fallback_type : builtins.void_type;
    if (purified.size() == 1) {
      // Validate fallback type matches the purified type (if non-empty block).
      if (fallback_type && !is_invalid_type(fallback_type) &&
          !types_equal(fallback_type, builtins.void_type) &&
          !block.stmts.empty()) {
        expect_assignable(node.fallback->span, purified[0], fallback_type,
                          "or fallback");
      }
      return purified[0];
    }
    TypePtr result = make_union_type(std::move(purified));
    return result;
  }

  return expr_type;
}

TypePtr Analyzer::check_func_expr(const FuncExprNode &node,
                                  const Node &parent) {
  push_scope(ScopeKind::Function);

  if (node.generic)
    enter_generics(*node.generic);

  auto fn_type = resolve_signature(node.signature);

  if (node.signature.return_type)
    current_scope->return_types.push_back(resolve_type(*node.signature.return_type));

  // Re-declare parameters into the type-checking scope.
  for (auto &p : node.signature.params) {
    auto pt = resolve_type(*p.type);
    for (auto &ident : p.names.identifiers) {
      auto param_type = p.is_variadic ? make_array_type(pt) : pt;
      current_scope->symbols.emplace(
          std::string(ident.name),
          Symbol::parameter(std::string(ident.name), param_type, ident.span));
    }
  }

  auto &block = std::get<BlockNode>(node.body->data);
  auto body_type = check_block(block);

  // Check tail expression matches return type (unless tail always returns).
  auto &fn_info = std::get<FuncTypeInfo>(fn_type->detail);
  bool tail_is_return =
      !block.stmts.empty() && always_returns(*block.stmts.back());
  if (!tail_is_return && fn_info.return_type &&
      !is_invalid_type(body_type)) {
    if (!types_equal(fn_info.return_type, builtins.void_type)) {
      expect_assignable(node.body->span, fn_info.return_type, body_type,
                        "return type");
    }
  }

  pop_scope();

  // Update capture types now that type checking is complete.
  // During resolve phase, capture types may have been nullptr.
  auto &caps_map =
      current_instantiation_ ? current_instantiation_->node_captures
                             : node_captures;
  auto cap_it = caps_map.find(&parent);
  if (cap_it != caps_map.end()) {
    for (auto &cap : cap_it->second) {
      if (!cap.type) {
        auto sym = lookup(cap.name);
        if (sym)
          cap.type = sym->type;
      }
    }
  }

  return fn_type;
}

TypePtr Analyzer::check_import_expr(const ImportExprNode &node) {
  // Import expressions used as `const X = import "path"` are already
  // processed during the import phase.  If we reach here, look up the
  // resolved module type from the cache.
  std::string path(node.path);
  if (package_resolver) {
    auto cache_it = package_resolver->cache.find(path);
    if (cache_it != package_resolver->cache.end()) {
      return cache_it->second;
    }
    auto mock_it = package_resolver->mock_packages.find(path);
    if (mock_it != package_resolver->mock_packages.end()) {
      return mock_it->second;
    }
  }
  // Expected to have been reported when the import failed; poison() checks
  // that claim rather than trusting it.
  return poison(node.span, "import '" + path + "' resolved to no package");
}

// ===========================================================================
// Statement type-checking
// ===========================================================================

void Analyzer::check_stmt(const Node &node) {
  std::visit(overloaded{
                 [&](const VarDeclNode &n) { check_var_decl(n, node); },
                 [&](const DeclAssignNode &n) { check_decl_assign(n); },
                 [&](const AssignNode &n) { check_assign(n); },
                 [&](const IncrementNode &n) { check_increment(n); },
                 [&](const DecrementNode &n) { check_decrement(n); },
                 [&](const ReturnNode &n) { check_return(n); },
                 [&](const BreakNode &n) { check_break(n); },
                 [&](const NextNode &) { check_next({}); },
                 [&](const auto &) { check_expr(node); },
             },
             node.data);
}

void Analyzer::check_var_decl(const VarDeclNode &var, const Node &parent) {
  TypePtr declared_type = nullptr;
  if (var.type) {
    declared_type = resolve_type(**var.type);
    // Record the resolved type for the type annotation node so codegen
    // can look it up after analysis (when scopes are no longer available).
    if (declared_type)
      record_type(**var.type, declared_type);
  }

  TypePtr final_type = declared_type;

  if (var.init) {
    TypePtr init_type;
    if (auto *for_node = std::get_if<ForExprNode>(&(*var.init)->data)) {
      init_type = check_for_expr(*for_node, declared_type);
      record_type(**var.init, init_type);
    } else {
      init_type = check_expr_expecting(**var.init, declared_type);
    }
    // An empty `[]` / `{}` adopts the declared type — its element type is a
    // hole and the declaration is the context that fills it. The hole only
    // yields to a declaration of the same shape, so `a int = []` still fails.
    if (declared_type && contains_unknown(init_type)) {
      TypePtr underlying = unwrap_alias(declared_type);
      if (underlying && init_type && underlying->kind == init_type->kind)
        init_type = declared_type;
    }
    if (declared_type && !is_invalid_type(init_type)) {
      expect_assignable((*var.init)->span, declared_type, init_type,
                        "variable initializer");
    }
    if (!final_type)
      final_type = resolve_binding_type(materialize_untyped(init_type),
                                        (*var.init)->span);
  }

  // Update or create the symbol in the current scope.
  std::string name(var.name.name);
  auto sym_it = current_scope->symbols.find(name);
  if (sym_it != current_scope->symbols.end()) {
    sym_it->second.type = final_type;
  } else {
    current_scope->symbols.emplace(
        name, Symbol::variable(name, final_type, var.name.span));
  }
}

// A binding is an inference hole's last chance to be filled: nothing after it
// supplies a type. Spec: "arr1 := [] // invalid, no inferrable type"
// (docs/language.md:601).
TypePtr Analyzer::resolve_binding_type(TypePtr type, Span span) {
  if (!contains_unknown(type))
    return type;
  error(span, type->kind == TypeKind::Map
                  ? "empty map literal: cannot infer key/value type without a "
                    "declared type"
                  : "empty array literal: cannot infer element type without a "
                    "declared type");
  return builtins.invalid_type;
}

void Analyzer::check_decl_assign(const DeclAssignNode &decl) {
  auto rhs_type = resolve_binding_type(
      materialize_untyped(check_expr(*decl.value)), decl.value->span);

  for (auto &ident : decl.targets.identifiers) {
    std::string name(ident.name);
    auto sym_it = current_scope->symbols.find(name);
    if (sym_it != current_scope->symbols.end()) {
      sym_it->second.type = rhs_type;
    } else {
      // Symbol was declared during name resolution in a different scope
      // tree.  Re-declare it here so type information propagates.
      current_scope->symbols.emplace(
          name, Symbol::variable(name, rhs_type, ident.span));
    }
  }
}

const IdentifierNode *Analyzer::target_root(const Node &target) {
  if (auto *id = std::get_if<IdentifierNode>(&target.data))
    return id;
  if (auto *sel = std::get_if<SelectorNode>(&target.data))
    return target_root(*sel->object);
  if (auto *idx = std::get_if<IndexExprNode>(&target.data))
    return target_root(*idx->object);
  return nullptr;
}

// A write needs somewhere to land. Without this the store is emitted against a
// temporary and silently discarded, so the statement compiles and does nothing.
void Analyzer::reject_immutable_target(const Node &target,
                                       std::string_view verb) {
  auto *root = target_root(target);
  if (!root) {
    error(target.span,
          std::format("cannot {} a temporary value: the write would be "
                      "discarded",
                      verb));
    return;
  }

  auto sym = lookup(std::string(root->name));
  if (!sym)
    return;

  if (sym->kind == SymbolKind::Constant) {
    error(target.span,
          std::format("cannot {} constant '{}'", verb, root->name));
    return;
  }

  if (sym->kind != SymbolKind::Variable && sym->kind != SymbolKind::Parameter)
    error(target.span,
          std::format("cannot {} '{}': not a variable", verb, root->name));
}

// Errors are immutable pure data: reject writing a field via `=`, a compound
// assignment, or `++`/`--`.
void Analyzer::reject_error_field_mutation(const Node &target) {
  if (auto *sel = std::get_if<SelectorNode>(&target.data))
    if (is_error_valued(check_expr(*sel->object)))
      error(target.span,
            "cannot assign to a field of an error (errors are immutable)");
}

void Analyzer::check_assign(const AssignNode &node) {
  // Check each target and value.
  for (size_t i = 0; i < node.targets.size(); ++i) {
    reject_immutable_target(*node.targets[i], "assign to");
    reject_error_field_mutation(*node.targets[i]);

    auto target_type = check_expr(*node.targets[i]);
    if (i < node.values.size()) {
      auto val_type = check_expr(*node.values[i]);

      if (node.op == Token::Kind::Assignment) {
        expect_assignable(node.values[i]->span, target_type, val_type,
                          "assignment");
      } else if (node.op == Token::Kind::AddAssignment &&
                 target_type->kind == TypeKind::String) {
        // String concatenation assignment: s += "..."
        expect_assignable(node.values[i]->span, builtins.string_type, val_type,
                          "string concatenation assignment");
      } else if (node.op == Token::Kind::DivAssignment) {
        // Division assignment: x /= y — division can fail (div by zero),
        // so validate numeric but note the impure semantics.
        if (!is_numeric(target_type)) {
          error(node.targets[i]->span,
                std::format("/= requires numeric type, got {}",
                            type_to_string(target_type)));
        }
        if (!is_invalid_type(val_type)) {
          expect_assignable(node.values[i]->span, target_type, val_type,
                            "division assignment");
        }
      } else {
        // Compound assignment: +=, -=, *=
        // Target must be numeric.
        if (!is_numeric(target_type)) {
          error(node.targets[i]->span,
                std::format("compound assignment requires numeric type, "
                            "got {}",
                            type_to_string(target_type)));
        }
        if (!is_invalid_type(val_type)) {
          expect_assignable(node.values[i]->span, target_type, val_type,
                            "compound assignment");
        }
      }
    }
  }
}

void Analyzer::check_increment(const IncrementNode &node) {
  reject_immutable_target(*node.operand, "increment");
  reject_error_field_mutation(*node.operand);
  auto t = check_expr(*node.operand);
  if (!is_invalid_type(t) && t->kind != TypeKind::Int) {
    error(node.span, std::format("increment requires integer type, got {}",
                                 type_to_string(t)));
  }
}

void Analyzer::check_decrement(const DecrementNode &node) {
  reject_immutable_target(*node.operand, "decrement");
  reject_error_field_mutation(*node.operand);
  auto t = check_expr(*node.operand);
  if (!is_invalid_type(t) && t->kind != TypeKind::Int) {
    error(node.span, std::format("decrement requires integer type, got {}",
                                 type_to_string(t)));
  }
}

void Analyzer::check_return(const ReturnNode &node) {
  auto func_scope = current_scope->nearest(ScopeKind::Function);
  if (!func_scope) {
    error(node.span, "'return' outside of function");
    return;
  }

  auto &expected = func_scope->return_types;
  if (!node.value) {
    // Bare return — function must be Void or have no return type.
    if (!expected.empty() && !types_equal(expected[0], builtins.void_type)) {
      error(node.span, "missing return value");
    }
    return;
  }

  if (expected.empty()) {
    error(node.span, "return has a value, expected none");
    return;
  }

  auto val_type = check_expr_expecting(*node.value, expected[0]);
  expect_assignable(node.value->span, expected[0], val_type, "return value");
}

void Analyzer::check_break(const BreakNode &node) {
  if (!current_scope->is_inside(ScopeKind::Loop)) {
    error(node.span, "'break' outside of loop");
    return;
  }
  for (auto &val : node.values) {
    auto t = check_expr(*val);
    if (!break_value_types_.empty())
      break_value_types_.back().push_back(t);
  }
}

void Analyzer::check_next(const NextNode &) {
  // Nothing to type-check for next.
}

// ===========================================================================
// Block type-checking
// ===========================================================================

TypePtr Analyzer::check_block(const BlockNode &block) {
  TypePtr last_type = builtins.void_type;
  for (auto &stmt : block.stmts) {
    last_type = check_expr(*stmt);
  }
  return last_type;
}

// ---------------------------------------------------------------------------
// always_returns — true if every control-flow path through the node ends
// with a `return` statement.
// ---------------------------------------------------------------------------

bool Analyzer::always_returns(const Node &node) const {
  return std::visit(overloaded{
                        [](const ReturnNode &) -> bool { return true; },

                        [&](const BlockNode &b) -> bool {
                          // A block always returns if its last statement always
                          // returns.
                          if (b.stmts.empty())
                            return false;
                          return always_returns(*b.stmts.back());
                        },

                        [&](const IfExprNode &n) -> bool {
                          // Both then and else must exist and both must always
                          // return.
                          if (!n.else_block)
                            return false;
                          bool then_ret = always_returns(*n.then_block);
                          bool else_ret = always_returns(**n.else_block);
                          return then_ret && else_ret;
                        },

                        [&](const SwitchExprNode &n) -> bool {
                          // Every arm must always return, and there must be an
                          // else.
                          if (!n.else_body)
                            return false;
                          for (auto &arm : n.arms) {
                            if (!always_returns(*arm.body))
                              return false;
                          }
                          return always_returns(**n.else_body);
                        },

                        [](const auto &) -> bool { return false; },
                    },
                    node.data);
}

// ===========================================================================
// Top-level declaration type-checking
// ===========================================================================

void Analyzer::check_const_decl(const ConstDeclNode &c) {
  auto sym_it = current_scope->symbols.find(std::string(c.name.name));

  // `const Name = import "..."` is a named-import binding (bound in
  // process_imports), not a value constant.
  if (std::get_if<ImportExprNode>(&c.value->data))
    return;

  TypePtr declared_type = nullptr;
  if (c.type)
    declared_type = resolve_type(**c.type);

  auto init_type = check_expr(*c.value);

  if (!require_const_expr(*c.value)) {
    if (sym_it != current_scope->symbols.end())
      sym_it->second.type = builtins.invalid_type;
    return;
  }

  if (declared_type && !is_invalid_type(init_type))
    expect_assignable(c.value->span, declared_type, init_type,
                      "constant initializer");

  // Forward-ref initializers (e.g. `const A = B * 2` with B declared
  // later) leave init_type as Error.  Spec: zero value with no
  // diagnostic.  Fall back to Int so downstream method dispatch
  // (`A.String()`) and codegen find the right ABI.
  TypePtr const_type = declared_type             ? declared_type
                       : is_invalid_type(init_type) ? builtins.int_type
                                                  : init_type;

  if (sym_it != current_scope->symbols.end())
    sym_it->second.type = const_type;

  if (auto cv = evaluate_constant(*c.value))
    const_decl_values_[std::string(c.name.name)] = *cv;
}

bool Analyzer::reject_const(Span span, const std::string &message) {
  error(span, message);
  return false;
}

static bool string_has_interpolation(const StringLiteralNode &s) {
  for (auto &frag : s.fragments)
    if (!std::get_if<StringFragmentNode>(&frag->data))
      return true;
  return false;
}

bool Analyzer::const_ident_is_value(const IdentifierNode &id, Span span) {
  auto sym = lookup(std::string(id.name));
  if (!sym)
    return reject_const(span, std::format("undefined name '{}'", id.name));
  switch (sym->kind) {
  case SymbolKind::Constant:
  case SymbolKind::EnumVariant:
    return true;
  case SymbolKind::Type:
  case SymbolKind::TypeParam:
    return reject_const(
        span, "a constant must be a value, not a type; use `type` to "
              "declare an alias");
  default:
    return reject_const(
        span, std::format("'{}' is not a compile-time constant", id.name));
  }
}

bool Analyzer::const_selector_is_enum_variant(const SelectorNode &sel,
                                              Span span) {
  if (auto *base = std::get_if<IdentifierNode>(&sel.object->data)) {
    if (auto sym = lookup(std::string(base->name)); sym && sym->type) {
      auto t = unwrap_alias(sym->type);
      if (t->kind == TypeKind::Enum) {
        auto &info = std::get<EnumTypeInfo>(t->detail);
        for (auto &v : info.variants)
          if (v.name == sel.field.name)
            return true;
      }
    }
  }
  return reject_const(
      span, "a selector is not a compile-time constant unless it names an "
            "enum variant");
}

bool Analyzer::require_const_expr(const Node &expr) {
  return std::visit(
      overloaded{
          [](const BoolLiteralNode &) { return true; },
          [](const NullLiteralNode &) { return true; },
          [](const IntegerLiteralNode &) { return true; },
          [](const FloatLiteralNode &) { return true; },
          [&](const StringLiteralNode &s) {
            return string_has_interpolation(s)
                       ? reject_const(expr.span,
                                      "an interpolated string is not a "
                                      "compile-time constant")
                       : true;
          },
          [&](const GroupExprNode &g) { return require_const_expr(*g.inner); },
          [&](const UnaryExprNode &u) { return require_const_expr(*u.operand); },
          [&](const BinaryExprNode &b) {
            return require_const_expr(*b.lhs) && require_const_expr(*b.rhs);
          },
          [&](const ArrayLiteralNode &a) {
            for (auto &el : a.elements)
              if (!require_const_expr(*el))
                return false;
            return true;
          },
          [&](const StructLiteralNode &s) {
            for (auto &f : s.fields)
              if (!require_const_expr(*f.value))
                return false;
            return true;
          },
          [&](const IdentifierNode &id) {
            return const_ident_is_value(id, expr.span);
          },
          [&](const SelectorNode &sel) {
            return const_selector_is_enum_variant(sel, expr.span);
          },
          [&](const MapLiteralNode &) {
            return reject_const(expr.span,
                                "maps cannot be constants — they need runtime "
                                "construction; use a package-level variable");
          },
          [&](const CallExprNode &) {
            return reject_const(
                expr.span, "a function call is not a compile-time constant");
          },
          [&](const FuncExprNode &) {
            return reject_const(
                expr.span, "a function value is not a compile-time constant");
          },
          [&](const IndexExprNode &) {
            return reject_const(
                expr.span,
                "an indexing expression is not a compile-time constant");
          },
          [&](const SliceNode &) {
            return reject_const(expr.span,
                                "a slice is not a compile-time constant");
          },
          [&](const IfExprNode &) {
            return reject_const(
                expr.span, "an `if` expression is not a compile-time constant");
          },
          [&](const SwitchExprNode &) {
            return reject_const(
                expr.span,
                "a `switch` expression is not a compile-time constant");
          },
          [&](const ForExprNode &) {
            return reject_const(
                expr.span,
                "a `for` expression is not a compile-time constant");
          },
          [&](const SpawnExprNode &) {
            return reject_const(
                expr.span,
                "a `spawn` expression is not a compile-time constant");
          },
          [&](const OrExprNode &) {
            return reject_const(
                expr.span, "an `or` expression is not a compile-time constant");
          },
          [&](const IsExpr &) {
            return reject_const(
                expr.span, "an `is` expression is not a compile-time constant");
          },
          [&](const auto &) {
            return reject_const(expr.span,
                                "this expression is not a compile-time "
                                "constant");
          },
      },
      expr.data);
}

void Analyzer::check_enum_decl(const EnumDeclNode &e) {
  auto sym = lookup(std::string(e.name.name));
  if (!sym || !sym->type || sym->type->kind != TypeKind::Enum)
    return;
  auto &info = std::get<EnumTypeInfo>(sym->type->detail);

  std::unordered_set<std::string> seen_names;
  std::unordered_map<int64_t, std::string> seen_index;
  std::unordered_map<std::string, std::string> seen_string;
  for (size_t i = 0; i < e.fields.size(); ++i) {
    auto &field = e.fields[i];
    std::string vname(field.name.name);
    if (!seen_names.insert(vname).second)
      error(field.name.span, std::format("duplicate enum variant '{}'", vname));

    if (e.string_backed) {
      if (field.value && !plain_string_literal(field.value))
        error(field.value->span,
              std::format("string-backed enum variant '{}' backing value must "
                          "be a string literal",
                          vname));
      if (i < info.variants.size()) {
        auto &sv = info.variants[i].string_value;
        auto [it, inserted] = seen_string.try_emplace(sv, vname);
        if (!inserted)
          error(field.name.span,
                std::format("enum variant '{}' backing string \"{}\" already "
                            "used by '{}'",
                            vname, sv, it->second));
      }
      continue;
    }

    if (field.value && !std::get_if<IntegerLiteralNode>(&field.value->data))
      error(field.value->span,
            std::format("enum variant '{}' backing value must be an integer "
                        "literal",
                        vname));

    if (i < info.variants.size()) {
      int64_t idx = info.variants[i].index;
      auto [it, inserted] = seen_index.try_emplace(idx, vname);
      if (!inserted)
        error(field.name.span,
              std::format("enum variant '{}' has index {} already used by '{}'",
                          vname, idx, it->second));
    }
  }

  auto methods = type_methods_.find(sym->type.get());
  if (methods != type_methods_.end())
    check_method_uniqueness(methods->second, "enum", info.name, e.name.span,
                            {"Int", "String", "From"});
}

TypePtr Analyzer::check_enum_shorthand(const EnumShorthandNode &sh,
                                       const Node &node,
                                       const TypePtr &expected) {
  TypePtr enum_type = expected ? unwrap_alias(expected) : nullptr;
  if (!enum_type || enum_type->kind != TypeKind::Enum) {
    error(sh.span,
          std::format("'.{}' shorthand needs a known enum type here",
                      sh.variant.name));
    return builtins.invalid_type;
  }
  auto &info = std::get<EnumTypeInfo>(enum_type->detail);
  for (auto &v : info.variants)
    if (v.name == sh.variant.name) {
      record_type(node, enum_type);
      return enum_type;
    }
  error(sh.variant.span,
        std::format("enum '{}' has no variant '{}'", info.name,
                    sh.variant.name));
  return builtins.invalid_type;
}

TypePtr Analyzer::check_expr_expecting(const Node &expr,
                                       const TypePtr &expected) {
  if (auto *sh = std::get_if<EnumShorthandNode>(&expr.data))
    return check_enum_shorthand(*sh, expr, expected);
  // An empty collection literal carries no element type of its own, so it
  // takes the one being asked for.
  if (auto exp = unwrap_alias(expected)) {
    if (auto *arr = std::get_if<ArrayLiteralNode>(&expr.data);
        arr && arr->elements.empty() && exp->kind == TypeKind::Array) {
      record_type(expr, exp);
      return exp;
    }
    if (auto *map = std::get_if<MapLiteralNode>(&expr.data);
        map && map->entries.empty() && exp->kind == TypeKind::Map) {
      record_type(expr, exp);
      return exp;
    }
  }
  return check_expr(expr);
}

void Analyzer::check_type_decl(const TypeDeclNode &t) {
  auto sym = lookup(std::string(t.name.name));
  if (!sym || !sym->type || sym->type->kind != TypeKind::Alias)
    return;
  auto &info = std::get<AliasTypeInfo>(sym->type->detail);
  if (info.structural)
    return;
  check_method_uniqueness(info.methods, "type", info.name, t.name.span, {});
}

void Analyzer::check_func_decl(const FuncDeclNode &fn) {
  // The body is checked via check_func_decl_body.
}

void Analyzer::check_method_uniqueness(
    const std::vector<MethodInfo> &methods, std::string_view kind,
    std::string_view name, Span span,
    const std::unordered_set<std::string> &reserved) {
  std::unordered_set<std::string> seen;
  for (auto &m : methods) {
    if (reserved.count(m.name)) {
      error(span, std::format("cannot redefine built-in method '{}' on {} '{}'",
                              m.name, kind, name));
      continue;
    }
    if (!seen.insert(m.name).second)
      error(span, std::format("duplicate method '{}' in {} '{}'", m.name, kind,
                              name));
  }
}

void Analyzer::check_struct_decl(const StructDeclNode &s) {
  // Struct fields and methods are resolved in Phase 2b.  Here we check
  // for duplicate field names and duplicate method names.
  auto sym = lookup(std::string(s.name.name));
  if (!sym || !sym->type || sym->type->kind != TypeKind::Struct)
    return;

  auto &info = std::get<StructTypeInfo>(sym->type->detail);

  // Check duplicate fields.
  std::unordered_map<std::string, bool> seen_fields;
  for (auto &f : info.fields) {
    if (seen_fields.count(f.name)) {
      error(s.span, std::format("duplicate field '{}' in struct '{}'", f.name,
                                info.name));
    }
    seen_fields[f.name] = true;
  }

  check_method_uniqueness(info.methods, "struct", info.name, s.span, {});

  check_no_infinite_size(sym->type, s.span);
  check_field_defaults(s);
}

// A struct holds its fields inline, so one that reaches itself that way has no
// finite size and no base case to stop at. An array, a map, or a union slot
// holding a boxed alternative puts the value on the heap, which is where a
// recursive shape gets its footing.
static bool reaches_by_value(const TypePtr &origin, const TypePtr &t,
                             std::unordered_set<const Type *> &visiting) {
  auto u = unwrap_alias(t);
  if (!u)
    return false;
  if (same_struct_decl(u, origin))
    return true;
  if (u->kind == TypeKind::Union) {
    for (auto &alt : std::get<UnionTypeInfo>(u->detail).alternatives)
      if (!union_alt_is_boxed(alt) && reaches_by_value(origin, alt, visiting))
        return true;
    return false;
  }
  if (u->kind != TypeKind::Struct)
    return false;
  if (!visiting.insert(u.get()).second)
    return false;
  auto &si = std::get<StructTypeInfo>(u->detail);
  for (auto &f : si.fields)
    if (reaches_by_value(origin, f.type, visiting))
      return true;
  for (auto &e : si.embeds)
    if (reaches_by_value(origin, e, visiting))
      return true;
  return false;
}

void Analyzer::check_no_infinite_size(const TypePtr &struct_type, Span span) {
  auto &info = std::get<StructTypeInfo>(struct_type->detail);
  auto blame = [&](const std::string &member) {
    error(span,
          std::format("'{}' contains itself through '{}', so it has no finite "
                      "size; give it a way to stop — '{} | Missing', an array, "
                      "or a map",
                      info.name, member, info.name));
  };
  for (auto &f : info.fields) {
    std::unordered_set<const Type *> visiting{struct_type.get()};
    if (reaches_by_value(struct_type, f.type, visiting))
      return blame(f.name);
  }
  for (auto &e : info.embeds) {
    std::unordered_set<const Type *> visiting{struct_type.get()};
    if (reaches_by_value(struct_type, e, visiting))
      return blame(type_to_string(e));
  }
}

void Analyzer::check_field_defaults(const StructDeclNode &s) {
  for (auto &member : s.members) {
    if (auto *fs = std::get_if<FieldSpecNode>(&member.member->data))
      check_field_default(*fs);
  }
}

// A field default must be a comptime expression assignable to the field type.
void Analyzer::check_field_default(const FieldSpecNode &fs) {
  if (!fs.default_value)
    return;

  auto field_type = resolve_type(*fs.type);
  auto def_type = check_expr(*fs.default_value);
  if (!require_const_expr(*fs.default_value))
    return;
  if (!field_type || is_invalid_type(def_type))
    return;

  std::string fname = fs.names.identifiers.empty()
                          ? "field"
                          : std::string(fs.names.identifiers.front().name);
  expect_assignable(fs.default_value->span, field_type, def_type,
                    std::format("default for field '{}'", fname));
}

void Analyzer::check_error_decl(const ErrorDeclNode &e) {
  auto sym = lookup(std::string(e.name.name));
  if (!sym || !sym->type || sym->type->kind != TypeKind::Struct)
    return;
  auto &info = std::get<StructTypeInfo>(sym->type->detail);

  // `Missing` and `Trapped` are built-in errors with reserved type_ids; a
  // user redeclaration would alias them (breaking `is`/`==` identity).
  if (info.name == "Missing" || info.name == "Trapped")
    error(e.name.span,
          std::format("'{}' is a reserved built-in error type", info.name));

  std::unordered_map<std::string, bool> seen;
  for (auto &f : info.fields) {
    if (seen.count(f.name))
      error(e.span, std::format("duplicate field '{}' in error '{}'", f.name,
                                info.name));
    seen[f.name] = true;
  }

  if (e.message_default)
    check_error_message_default(info, *e.message_default);

  for (auto &member : e.members) {
    if (auto *fs = std::get_if<FieldSpecNode>(&member.member->data))
      check_field_default(*fs);
  }
}

// The message default is checked with the error's own fields in scope, so a
// `message = "code {code}"` default can interpolate them. It must be a string
// literal (which may interpolate self-fields) or a comptime constant — not an
// arbitrary runtime expression.
void Analyzer::check_error_message_default(const StructTypeInfo &info,
                                           const Node &msg_default) {
  push_scope(ScopeKind::Block);
  for (auto &f : info.fields) {
    if (f.name == "type_id" || f.name == "message")
      continue;
    current_scope->symbols[f.name] =
        Symbol::variable(f.name, f.type, msg_default.span);
  }
  // The message default isn't reached by the normal resolve pass (its scope is
  // the error's fields, not a lexical scope), so resolve it here to report
  // undefined names before the interpolation silently drops them.
  resolve_expr(msg_default);
  auto def_type = check_expr(msg_default);
  pop_scope();

  // A self-interpolating string literal isn't a compile-time constant but is
  // allowed; any other non-constant expression (e.g. a call) is rejected.
  if (!std::holds_alternative<StringLiteralNode>(msg_default.data) &&
      !require_const_expr(msg_default))
    return;

  if (!is_invalid_type(def_type))
    expect_assignable(msg_default.span, builtins.string_type, def_type,
                      "error message default");
}

void Analyzer::check_interface_decl(const InterfaceDeclNode &i) {
  auto sym = lookup(std::string(i.name.name));
  if (!sym || !sym->type || sym->type->kind != TypeKind::Interface)
    return;

  auto &info = std::get<InterfaceTypeInfo>(sym->type->detail);

  if (info.methods.empty()) {
    error(i.span,
          std::format("interface '{}' must declare at least one method "
                      "(directly or through an embedded interface)",
                      info.name));
  }

  check_method_uniqueness(info.methods, "interface", info.name, i.span, {});
}

void Analyzer::check_import_decl(const ImportDeclNode &node) {
  // Import declarations are fully processed during the import phase (1.5).
  // Here we just verify the module symbol was successfully resolved.
  std::string path(node.path);
  auto last_slash = path.rfind('/');
  std::string name =
      (last_slash != std::string::npos) ? path.substr(last_slash + 1) : path;
  auto sym = lookup(name);
  if (sym && sym->type && is_invalid_type(sym->type)) {
    // Error was already reported during resolve_import.
  }
}

// ===========================================================================
// Generic instantiation
// ===========================================================================

TypePtr
Analyzer::instantiate_generic_call(
    const TypePtr &callee_type, const std::vector<TypePtr> &arg_types,
    Span call_span, std::unordered_map<uint32_t, TypePtr> *out_bindings) {
  if (!callee_type || callee_type->kind != TypeKind::Func)
    return builtins.invalid_type;

  auto &fn_info = std::get<FuncTypeInfo>(callee_type->detail);
  std::unordered_map<uint32_t, TypePtr> bindings;

  // Attempt to unify each parameter type with the argument type.
  size_t count = std::min(fn_info.params.size(), arg_types.size());
  for (size_t i = 0; i < count; ++i) {
    if (!unify(fn_info.params[i], arg_types[i], bindings)) {
      error(call_span,
            std::format("cannot infer type parameter from argument {}", i + 1));
      return builtins.invalid_type;
    }
  }

  // Validate each binding against the type-parameter's constraint, if any.
  // Constraints are carried on the TypeParam nodes embedded in the function's
  // parameter/return types — walk the type tree to recover them.
  std::unordered_map<uint32_t, TypeConstraint> constraints;
  std::unordered_map<uint32_t, TypePtr> bounds;
  auto collect = [&](auto &self, const TypePtr &t) -> void {
    if (!t) return;
    if (t->kind == TypeKind::TypeParam) {
      auto &info = std::get<TypeParamInfo>(t->detail);
      if (info.param.constraint != TypeConstraint::None)
        constraints[info.param.id] = info.param.constraint;
      if (info.bound)
        bounds[info.param.id] = *info.bound;
      return;
    }
    switch (t->kind) {
    case TypeKind::Array:
      self(self, std::get<ArrayTypeInfo>(t->detail).element);
      break;
    case TypeKind::Map: {
      auto &m = std::get<MapTypeInfo>(t->detail);
      self(self, m.key);
      self(self, m.value);
      break;
    }
    case TypeKind::Func: {
      auto &f = std::get<FuncTypeInfo>(t->detail);
      for (auto &p : f.params) self(self, p);
      if (f.return_type) self(self, f.return_type);
      break;
    }
    case TypeKind::Union:
      for (auto &a : std::get<UnionTypeInfo>(t->detail).alternatives)
        self(self, a);
      break;
    default:
      break;
    }
  };
  for (auto &p : fn_info.params) collect(collect, p);
  if (fn_info.return_type) collect(collect, fn_info.return_type);

  bool constraint_violation = false;
  for (auto &[id, concrete] : bindings) {
    auto it = constraints.find(id);
    if (it == constraints.end()) continue;
    if (!satisfies_constraint(concrete, it->second)) {
      error(call_span,
            std::format("type {} does not satisfy constraint {}",
                        type_to_string(concrete),
                        constraint_name(it->second)));
      constraint_violation = true;
    }
  }
  for (auto &[id, concrete] : bindings) {
    auto it = bounds.find(id);
    if (it == bounds.end()) continue;
    if (!concrete || is_invalid_type(concrete) ||
        concrete->kind == TypeKind::TypeParam)
      continue;
    auto &ii = std::get<InterfaceTypeInfo>(it->second->detail);
    if (!report_interface_unsatisfied(concrete, it->second, ii.name, call_span,
                                      ""))
      constraint_violation = true;
  }
  if (constraint_violation)
    return poison(call_span, "constraint violation left unreported");

  if (out_bindings)
    *out_bindings = bindings;

  if (bindings.empty())
    return callee_type; // No type params to substitute.

  return substitute(callee_type, bindings);
}

Analyzer::BodyInstantiation *Analyzer::instantiate_generic_body(
    const FuncDeclNode &fn,
    const std::unordered_map<uint32_t, TypePtr> &bindings,
    const Node &call_node) {
  auto tpl_it = generic_templates_.find(&fn);
  if (tpl_it == generic_templates_.end())
    return nullptr; // not a generic free function we know about

  auto &tpl = tpl_it->second;

  // Reuse a cached instantiation with matching bindings.  A matching entry
  // that is still in_progress is the recursion guard for mutually recursive
  // generics: we hand back the partial entry without re-analysing.
  auto &list = instantiations_[&fn];
  auto same_bindings = [&](const std::unordered_map<uint32_t, TypePtr> &a,
                           const std::unordered_map<uint32_t, TypePtr> &b) {
    if (a.size() != b.size())
      return false;
    for (auto &[id, t] : a) {
      auto it = b.find(id);
      if (it == b.end())
        return false;
      if (!types_equal(t, it->second))
        return false;
    }
    return true;
  };
  for (auto &inst : list) {
    if (same_bindings(inst.bindings, bindings))
      return &inst;
  }

  // Fresh entry.  Insert with in_progress = true so a re-entrant call from
  // the body analysis (same decl, same bindings) finds the partial entry
  // and breaks the cycle.
  list.push_back(BodyInstantiation{});
  BodyInstantiation &inst = list.back();
  inst.decl = &fn;
  inst.bindings = bindings;
  inst.in_progress = true;

  // Swap to a fresh child of the declaration's lexical scope.  Names in
  // the body resolve against where the generic was declared, not the
  // caller's scope.
  auto saved_scope = current_scope;
  BodyInstantiation *saved_inst = current_instantiation_;
  bool saved_is_stdlib = is_stdlib;
  instantiation_stack_.push_back(&call_node);

  current_scope = tpl.decl_scope->child(ScopeKind::Block);
  // Stdlib bodies (e.g. Map.String() in std/map) re-analysed at a user-
  // package call site keep their stdlib provenance so intrinsic_* calls
  // inside the body remain allowed.
  if (tpl.is_stdlib)
    is_stdlib = true;

  // Register each generic type parameter under its original name in the
  // new scope so a reference to `T` inside the body resolves to the
  // concrete binding.  Both the named symbol lookup and the type_bindings
  // table (used by substitute()) are populated.
  for (auto &tp : tpl.type_params) {
    auto bind_it = bindings.find(tp.id);
    if (bind_it == bindings.end())
      continue;
    auto &concrete = bind_it->second;
    current_scope->symbols.emplace(
        tp.name, Symbol::type_param(tp.name, concrete, Span{}));
    current_scope->type_bindings[tp.id] = concrete;
  }

  // Push the function scope that will hold parameters and return types.
  push_scope(ScopeKind::Function);

  // Inject receiver parameter for generic methods on concrete types.
  if (fn.receiver) {
    auto recv_type = resolve_type(*fn.receiver->type);
    declare_local(Symbol::parameter(std::string(fn.receiver->name.name),
                                    recv_type, fn.receiver->name.span));
  }

  // Substituted return type for return-stmt checking.
  if (fn.signature.return_type) {
    auto rt = resolve_type(*fn.signature.return_type);
    current_scope->return_types.push_back(substitute(rt, bindings));
  }

  // Inject parameters with substituted (concrete) types.  Without this
  // step, identifiers referring to parameters inside the body would
  // resolve to their original TypeParam-typed symbols and the body would
  // type-check against `T` rather than the concrete type.
  for (auto &p : fn.signature.params) {
    auto pt = resolve_type(*p.type);
    pt = substitute(pt, bindings);
    for (auto &ident : p.names.identifiers) {
      auto param_type = p.is_variadic ? make_array_type(pt) : pt;
      declare_local(Symbol::parameter(std::string(ident.name),
                                      std::move(param_type), ident.span));
    }
  }

  // Route side-table writes into this instantiation's view.
  current_instantiation_ = &inst;

  // Phase 3 — name resolution over the body.
  auto &block = std::get<BlockNode>(fn.body->data);
  resolve_block(block);

  // Phase 4 — type checking over the body.
  auto body_type = check_block(block);

  // Tail-expression return compatibility (mirrors check_func_decl_body).
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

  // Restore everything.
  pop_scope();           // the Function scope
  current_scope = saved_scope;
  current_instantiation_ = saved_inst;
  is_stdlib = saved_is_stdlib;
  instantiation_stack_.pop_back();

  inst.in_progress = false;
  return &inst;
}

TypePtr Analyzer::instantiate_generic_struct(
    const TypePtr &struct_type,
    const std::vector<std::pair<std::string, TypePtr>> &field_types,
    Span span) {
  if (!struct_type || struct_type->kind != TypeKind::Struct)
    return poison(span, "generic instantiation of a non-struct type");

  auto &info = std::get<StructTypeInfo>(struct_type->detail);
  if (info.type_params.empty())
    return struct_type; // Not generic.

  std::unordered_map<uint32_t, TypePtr> bindings;

  // Unify each provided field value type against the struct's field type. A
  // field may bind nothing — `tail Node<T> | Missing` given `Missing{}` says
  // nothing about T — which is silent here; an unbound parameter is caught
  // below by the field that was supposed to bind it.
  for (auto &[fname, ftype] : field_types) {
    for (auto &fi : info.fields) {
      if (fi.name == fname && fi.type) {
        unify(fi.type, ftype, bindings);
        break;
      }
    }
  }

  if (bindings.empty())
    return struct_type;

  // Publish the shell before substituting fields, and map the declaration onto
  // it, so a field naming the struct again lands on this instantiation.
  auto result = make_struct_type(info.name, {}, {}, info.type_params,
                                 info.origin_package);
  auto &result_info = std::get<StructTypeInfo>(result->detail);
  SubstMemo memo{{struct_type.get(), result}};

  for (auto &f : info.fields)
    result_info.fields.push_back(
        {f.name, substitute(f.type, bindings, memo), f.is_public,
         f.default_value});
  for (auto &m : info.methods)
    result_info.methods.push_back(
        {m.name, substitute(m.signature, bindings, memo), m.is_public,
         m.origin_package});
  // Record the concrete type arguments.
  for (auto &tp : info.type_params) {
    auto it = bindings.find(tp.id);
    if (it != bindings.end())
      result_info.type_args.push_back(it->second);
  }
  result_info.embeds = info.embeds;

  return result;
}

// ===========================================================================
// Interface conformance
// ===========================================================================

// signatures_match_with_self — compare an interface method signature against
// a concrete method signature, treating any reference to `iface_self` inside
// `iface_sig` as matching `concrete_self` in `concrete_sig`.  This is the
// Go-style trick that lets `Equals(Hashable) Bool` declared in `interface
// Hashable` match `Equals(Int) Bool` on the Int receiver.  Outside the
// self-reference position, signatures must be `types_equal`.
static bool signatures_match_with_self(const TypePtr &iface_sig,
                                       const TypePtr &concrete_sig,
                                       const TypePtr &iface_self,
                                       const TypePtr &concrete_self) {
  if (!iface_sig || !concrete_sig)
    return false;
  if (iface_sig->kind != TypeKind::Func ||
      concrete_sig->kind != TypeKind::Func)
    return false;

  auto &a = std::get<FuncTypeInfo>(iface_sig->detail);
  auto &b = std::get<FuncTypeInfo>(concrete_sig->detail);
  if (a.params.size() != b.params.size())
    return false;
  if (static_cast<bool>(a.return_type) != static_cast<bool>(b.return_type))
    return false;
  if (a.is_variadic != b.is_variadic)
    return false;

  // An interface position counts as "the interface itself" when it points
  // to the same nominal interface (same name + origin package).  SGI loads
  // produce fresh TypePtrs, so pointer equality alone is insufficient.
  auto refers_to_self = [&](const TypePtr &t) -> bool {
    if (!t || t->kind != TypeKind::Interface || !iface_self ||
        iface_self->kind != TypeKind::Interface)
      return false;
    if (t.get() == iface_self.get())
      return true;
    auto &a = std::get<InterfaceTypeInfo>(t->detail);
    auto &b = std::get<InterfaceTypeInfo>(iface_self->detail);
    return a.name == b.name && a.origin_package == b.origin_package;
  };

  auto self_match = [&](const TypePtr &iv, const TypePtr &cv) -> bool {
    if (refers_to_self(iv))
      return types_equal(cv, concrete_self) ||
             (cv && concrete_self &&
              cv->kind == concrete_self->kind &&
              cv->kind == TypeKind::Interface);
    return types_equal(iv, cv);
  };

  for (size_t i = 0; i < a.params.size(); ++i) {
    if (!self_match(a.params[i], b.params[i]))
      return false;
  }
  if (a.return_type && !self_match(a.return_type, b.return_type))
    return false;
  return true;
}

// Gather the methods visible on `concrete` from the analyzer's stdlib and
// built-in tables.  Shared between satisfies_interface and the named-protocol
// diagnostic so missing-method reports stay aligned with what the structural
// match actually consulted.
static std::vector<MethodInfo> collect_concrete_methods(
    const TypePtr &concrete,
    const std::unordered_map<const Type *, std::vector<MethodInfo>>
        &type_methods,
    const std::unordered_map<TypeKind, std::vector<MethodInfo>> &kind_methods,
    const BuiltinTypes &builtins) {
  std::vector<MethodInfo> methods;
  if (!concrete)
    return methods;

  if (concrete->kind == TypeKind::Struct) {
    auto &s = std::get<StructTypeInfo>(concrete->detail);
    methods = s.methods;
    return methods;
  }

  const Type *raw = concrete.get();
  auto tm_it = type_methods.find(raw);
  if (tm_it == type_methods.end()) {
    const Type *canonical = nullptr;
    switch (concrete->kind) {
    case TypeKind::Int:    canonical = builtins.int_type.get(); break;
    case TypeKind::Float:  canonical = builtins.float_type.get(); break;
    case TypeKind::Bool:   canonical = builtins.bool_type.get(); break;
    case TypeKind::String: canonical = builtins.string_type.get(); break;
    default: break;
    }
    if (canonical && canonical != raw)
      tm_it = type_methods.find(canonical);
  }
  if (tm_it != type_methods.end()) {
    for (auto &m : tm_it->second)
      methods.push_back(m);
  }

  auto km_it = kind_methods.find(concrete->kind);
  if (km_it != kind_methods.end()) {
    for (auto &m : km_it->second)
      methods.push_back(m);
  }

  for (auto &m : builtin_methods(concrete->kind, builtins))
    methods.push_back(m);

  return methods;
}

bool Analyzer::satisfies_interface(const TypePtr &concrete,
                                   const TypePtr &iface) {
  if (!concrete || !iface)
    return false;
  if (iface->kind != TypeKind::Interface)
    return false;

  auto &iface_info = std::get<InterfaceTypeInfo>(iface->detail);
  auto concrete_methods =
      collect_concrete_methods(concrete, type_methods_, kind_methods_, builtins);

  // Every interface method must be present on the concrete type with a
  // compatible signature.  Self-references to `iface` inside an interface
  // method signature stand for the concrete receiver type.
  for (auto &im : iface_info.methods) {
    bool found = false;
    for (auto &cm : concrete_methods) {
      if (cm.name == im.name) {
        found = true;
        if (im.signature && cm.signature) {
          if (!signatures_match_with_self(im.signature, cm.signature, iface,
                                          concrete)) {
            return false;
          }
        }
        break;
      }
    }
    if (!found)
      return false;
  }

  return true;
}

// Render an interface method's signature into a single-line form suitable for
// citing in a "missing method" diagnostic, e.g. `Hash() Int64` or
// `Equals(Self) Bool`.  Self-typed positions (the protocol's own interface
// type appearing in its method signature) print as `Self` so the user reads
// the protocol contract rather than its declared `Equals(Hashable) Bool`
// shape.
static std::string format_protocol_method(const std::string &name,
                                          const TypePtr &sig,
                                          const TypePtr &iface) {
  if (!sig || sig->kind != TypeKind::Func)
    return name + "(...)";
  auto &fi = std::get<FuncTypeInfo>(sig->detail);
  auto &iinfo = std::get<InterfaceTypeInfo>(iface->detail);

  auto render = [&](const TypePtr &t) -> std::string {
    if (t && t->kind == TypeKind::Interface) {
      auto &ti = std::get<InterfaceTypeInfo>(t->detail);
      if (ti.name == iinfo.name && ti.origin_package == iinfo.origin_package)
        return "Self";
    }
    return type_to_string(t);
  };

  std::string s = name + "(";
  for (size_t i = 0; i < fi.params.size(); ++i) {
    if (i)
      s += ", ";
    s += render(fi.params[i]);
  }
  s += ")";
  if (fi.return_type)
    s += " " + render(fi.return_type);
  return s;
}

bool Analyzer::check_satisfies_protocol(const TypePtr &concrete,
                                         ProtocolKind p, Span at,
                                         const std::string &context) {
  if (!concrete || is_invalid_type(concrete))
    return true;
  // Inside a generic body the binding isn't known yet — the concrete check
  // happens at every monomorphisation site.
  if (concrete->kind == TypeKind::TypeParam)
    return true;

  TypePtr iface;
  const char *proto_name = nullptr;
  switch (p) {
  case ProtocolKind::Hashable:
    iface = builtins.hashable_iface;
    proto_name = "Hashable";
    break;
  case ProtocolKind::Stringable:
    iface = builtins.stringable_iface;
    proto_name = "Stringable";
    break;
  }
  // proto.sgi unavailable (bootstrap of std/proto, or no resolver) — skip.
  if (!iface)
    return true;

  // Float keys: NaN ≠ NaN breaks the consistency invariant, and the named
  // diagnostic should explain that rather than report a missing Hash().
  if (p == ProtocolKind::Hashable && concrete->kind == TypeKind::Float) {
    std::string ctx = context.empty() ? "" : (" (" + context + ")");
    error(at, std::format(
                  "{} is not Hashable: floats lack total equality "
                  "(NaN != NaN), which the hash-map consistency invariant "
                  "requires{}",
                  type_to_string(concrete), ctx));
    return false;
  }

  return report_interface_unsatisfied(concrete, iface, proto_name, at, context);
}

bool Analyzer::report_interface_unsatisfied(const TypePtr &concrete,
                                            const TypePtr &iface,
                                            const std::string &iface_name,
                                            Span at,
                                            const std::string &context) {
  if (satisfies_interface(concrete, iface))
    return true;

  // Build the "missing methods" list against the same method table the
  // structural matcher consulted.
  auto &iface_info = std::get<InterfaceTypeInfo>(iface->detail);
  auto concrete_methods =
      collect_concrete_methods(concrete, type_methods_, kind_methods_, builtins);

  std::vector<std::string> missing;
  for (auto &im : iface_info.methods) {
    bool ok = false;
    for (auto &cm : concrete_methods) {
      if (cm.name != im.name)
        continue;
      if (im.signature && cm.signature &&
          !signatures_match_with_self(im.signature, cm.signature, iface,
                                      concrete))
        break;
      ok = true;
      break;
    }
    if (!ok)
      missing.push_back(format_protocol_method(im.name, im.signature, iface));
  }

  std::string list;
  for (size_t i = 0; i < missing.size(); ++i) {
    if (i)
      list += ", ";
    list += missing[i];
  }
  std::string ctx = context.empty() ? "" : (" (" + context + ")");
  error(at, std::format(
                "type {} does not satisfy {}{}: missing {}",
                type_to_string(concrete), iface_name, ctx, list));
  return false;
}

bool Analyzer::check_stringable_recursive(const TypePtr &t, Span at,
                                          const std::string &context) {
  if (!t || is_invalid_type(t))
    return true;
  if (t->kind == TypeKind::Array) {
    auto &ai = std::get<ArrayTypeInfo>(t->detail);
    return check_stringable_recursive(ai.element, at, context);
  }
  if (t->kind == TypeKind::Map) {
    auto &mi = std::get<MapTypeInfo>(t->detail);
    bool k = check_stringable_recursive(mi.key, at, context);
    bool v = check_stringable_recursive(mi.value, at, context);
    return k && v;
  }
  return check_satisfies_protocol(t, ProtocolKind::Stringable, at, context);
}

// ---------------------------------------------------------------------------
// Constant-expression evaluator
// ---------------------------------------------------------------------------

namespace {

int64_t parse_int_literal_text(std::string_view lit) {
  std::string clean;
  clean.reserve(lit.size());
  for (char c : lit)
    if (c != '_') clean += c;

  int base = 10;
  std::string_view digits = clean;
  if (digits.size() > 2 && digits[0] == '0') {
    switch (digits[1]) {
      case 'b': case 'B': base = 2;  digits = digits.substr(2); break;
      case 'o': case 'O': base = 8;  digits = digits.substr(2); break;
      case 'x': case 'X': base = 16; digits = digits.substr(2); break;
      default: break;
    }
  }
  int64_t v = 0;
  std::from_chars(digits.data(), digits.data() + digits.size(), v, base);
  return v;
}

double parse_float_literal_text(std::string_view lit) {
  std::string clean;
  clean.reserve(lit.size());
  for (char c : lit)
    if (c != '_') clean += c;
  double v = 0.0;
  std::from_chars(clean.data(), clean.data() + clean.size(), v);
  return v;
}

} // namespace

std::optional<ConstValue> Analyzer::evaluate_constant(const Node &expr) {
  if (auto *il = std::get_if<IntegerLiteralNode>(&expr.data))
    return ConstValue::make_int(parse_int_literal_text(il->literal));

  if (auto *fl = std::get_if<FloatLiteralNode>(&expr.data))
    return ConstValue::make_float(parse_float_literal_text(fl->literal));

  if (auto *bl = std::get_if<BoolLiteralNode>(&expr.data))
    return ConstValue::make_bool(bl->literal == "true");

  if (auto *grp = std::get_if<GroupExprNode>(&expr.data))
    return evaluate_constant(*grp->inner);

  if (auto *un = std::get_if<UnaryExprNode>(&expr.data)) {
    auto inner = evaluate_constant(*un->operand);
    if (!inner) return std::nullopt;
    if (un->op == Token::Kind::Sub) {
      if (inner->kind == ConstValue::Kind::Int)
        return ConstValue::make_int(-inner->i);
      if (inner->kind == ConstValue::Kind::Float)
        return ConstValue::make_float(-inner->f);
    }
    if (un->op == Token::Kind::Not) {
      if (inner->kind == ConstValue::Kind::Bool)
        return ConstValue::make_bool(!inner->b);
    }
    if (un->op == Token::Kind::BitwiseNot) {
      if (inner->kind == ConstValue::Kind::Int)
        return ConstValue::make_int(~inner->i);
    }
    return std::nullopt;
  }

  if (auto *bin = std::get_if<BinaryExprNode>(&expr.data)) {
    auto lhs = evaluate_constant(*bin->lhs);
    auto rhs = evaluate_constant(*bin->rhs);
    if (!lhs || !rhs) return std::nullopt;
    if (lhs->kind != rhs->kind) return std::nullopt;

    if (lhs->kind == ConstValue::Kind::Int) {
      int64_t l = lhs->i, r = rhs->i;
      switch (bin->op) {
        case Token::Kind::Add:        return ConstValue::make_int(l + r);
        case Token::Kind::Sub:        return ConstValue::make_int(l - r);
        case Token::Kind::Multiply:   return ConstValue::make_int(l * r);
        case Token::Kind::Divide:
          if (r == 0) {
            error(expr.span, "division by zero in constant expression");
            return std::nullopt;
          }
          return ConstValue::make_int(l / r);
        case Token::Kind::Modulo:
          if (r == 0) {
            error(expr.span, "modulo by zero in constant expression");
            return std::nullopt;
          }
          return ConstValue::make_int(l % r);
        case Token::Kind::BitwiseAnd: return ConstValue::make_int(l & r);
        case Token::Kind::BitwiseOr:  return ConstValue::make_int(l | r);
        case Token::Kind::BitwiseXor: return ConstValue::make_int(l ^ r);
        case Token::Kind::LeftShift:
        case Token::Kind::RightShift:
          if (r < 0 || r >= 64) {
            error(expr.span,
                  std::format("shift count {} out of range [0, 64) in "
                              "constant expression", r));
            return std::nullopt;
          }
          return bin->op == Token::Kind::LeftShift
                     ? ConstValue::make_int(l << r)
                     : ConstValue::make_int(l >> r);
        default: return std::nullopt;
      }
    }
    if (lhs->kind == ConstValue::Kind::Float) {
      double l = lhs->f, r = rhs->f;
      switch (bin->op) {
        case Token::Kind::Add:      return ConstValue::make_float(l + r);
        case Token::Kind::Sub:      return ConstValue::make_float(l - r);
        case Token::Kind::Multiply: return ConstValue::make_float(l * r);
        case Token::Kind::Divide:
          if (r == 0.0) {
            error(expr.span, "division by zero in constant expression");
            return std::nullopt;
          }
          return ConstValue::make_float(l / r);
        default: return std::nullopt;
      }
    }
    return std::nullopt;
  }

  if (auto *id = std::get_if<IdentifierNode>(&expr.data)) {
    auto it = const_decl_values_.find(std::string(id->name));
    if (it != const_decl_values_.end())
      return it->second;
    return std::nullopt;
  }

  return std::nullopt;
}

} // namespace saga

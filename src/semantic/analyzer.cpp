// Copywrite 2026 Rob Thornton
// SPDX-License-Identifier: MIT

#include "semantic/analyzer.hpp"
#include "semantic/sgi.hpp"
#include "frontend/parser.hpp"

#include <algorithm>
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
    std::vector<TypePtr> params, rets;
    for (auto &p : info.params) {
      auto np = normalize_generic_receiver_sig(p, recv_kind);
      if (np != p) changed = true;
      params.push_back(std::move(np));
    }
    for (auto &r : info.returns) {
      auto nr = normalize_generic_receiver_sig(r, recv_kind);
      if (nr != r) changed = true;
      rets.push_back(std::move(nr));
    }
    if (!changed) return t;
    auto result = make_func_type(std::move(params), std::move(rets));
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
}

void Analyzer::load_prelude() {
  if (!package_resolver)
    return;

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
    if (type_name == "Float")  return builtins.float_type.get();
    if (type_name == "Bool")   return builtins.bool_type.get();
    if (type_name == "String") return builtins.string_type.get();
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

std::vector<TypeParam> Analyzer::enter_generics(const GenericNode &generic) {
  std::vector<TypeParam> params;
  for (auto &tp_node : generic.type_params) {
    auto &ident = std::get<IdentifierNode>(tp_node->data);
    uint32_t id = fresh_type_param_id();
    TypeParam tp{id, std::string(ident.name)};
    auto tp_type = make_type_param(id, tp.name);

    declare(Symbol::type_param(tp.name, tp_type, ident.span));
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
  Position pos{};
  if (!fileset.files.empty()) {
    pos = fileset.files[0]->position_at(span.start);
  }

  // Append an "...instantiated from" frame for every active generic
  // instantiation so multi-level generic errors render a C++-template-style
  // backtrace rather than a single opaque in-body location.
  std::string full = message;
  for (auto it = instantiation_stack_.rbegin();
       it != instantiation_stack_.rend(); ++it) {
    const Node *call_node = *it;
    if (!call_node)
      continue;
    Position frame_pos{};
    if (!fileset.files.empty()) {
      frame_pos = fileset.files[0]->position_at(call_node->span.start);
    }
    full += std::format("\n  ...instantiated from {}", frame_pos);
  }

  errors.report_error(pos, full);
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
  if (is_error_type(target_type) || is_error_type(value_type))
    return;
  if (!is_assignable_to(value_type, target_type)) {
    type_error(span, target_type, value_type, context);
  }
}

void Analyzer::expect_type(Span span, const TypePtr &type, TypeKind expected,
                           const std::string &context) {
  if (is_error_type(type))
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
      std::visit(overloaded{
                     [&](const StructDeclNode &s) { resolve_struct_decl(s); },
                     [&](const EnumDeclNode &e) { resolve_enum_decl(e); },
                     [&](const InterfaceDeclNode &i) { resolve_interface_decl(i); },
                     [&](const auto &) { /* handled in phase 2b */ },
                 },
                 decl->data);
    }
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
                     [&](const EnumDeclNode &) { /* done in phase 2a */ },
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
              [&](const StructDeclNode &s) {
                TypePtr struct_type = nullptr;
                auto sym = lookup(std::string(s.name.name));
                if (sym)
                  struct_type = sym->type;

                for (auto &member : s.members) {
                  if (auto *fn =
                          std::get_if<FuncDeclNode>(&member.member->data)) {
                    resolve_func_decl_body(*fn, struct_type);
                  }
                }
              },
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
              [&](const StructDeclNode &s) {
                check_struct_decl(s);

                TypePtr struct_type = nullptr;
                auto sym = lookup(std::string(s.name.name));
                if (sym)
                  struct_type = sym->type;

                for (auto &member : s.members) {
                  if (auto *fn =
                          std::get_if<FuncDeclNode>(&member.member->data)) {
                    check_func_decl_body(*fn, struct_type);
                  }
                }
              },
              [&](const EnumDeclNode &e) { check_enum_decl(e); },
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

  // Pass 3: resolve names inside function/method bodies.
  for (auto &decl : src.declarations) {
    std::visit(overloaded{
                   [&](const FuncDeclNode &fn) { resolve_func_decl_body(fn); },
                   [&](const StructDeclNode &s) {
                     TypePtr struct_type = nullptr;
                     auto sym = lookup(std::string(s.name.name));
                     if (sym)
                       struct_type = sym->type;

                     for (auto &member : s.members) {
                       if (auto *fn = std::get_if<FuncDeclNode>(
                               &member.member->data)) {
                         resolve_func_decl_body(*fn, struct_type);
                       }
                     }
                   },
                   [&](const auto &) {},
               },
               decl->data);
  }

  // Pass 4: type-check top-level declarations and function bodies.
  for (auto &decl : src.declarations) {
    std::visit(overloaded{
                   [&](const FuncDeclNode &fn) { check_func_decl_body(fn); },
                   [&](const StructDeclNode &s) {
                     check_struct_decl(s);

                     TypePtr struct_type = nullptr;
                     auto sym = lookup(std::string(s.name.name));
                     if (sym)
                       struct_type = sym->type;

                     for (auto &member : s.members) {
                       if (auto *fn = std::get_if<FuncDeclNode>(
                               &member.member->data)) {
                         check_func_decl_body(*fn, struct_type);
                       }
                     }
                   },
                   [&](const EnumDeclNode &e) { check_enum_decl(e); },
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
                   // Receiver methods are bound to a type; they're not
                   // callable as free functions and must not shadow types
                   // (e.g. Bool.String() must not shadow the String type).
                   if (!fn.receiver)
                     declare(Symbol::function(std::string(fn.name.name),
                                              nullptr, fn.name.span,
                                              fn.is_public));
                 },
                 [&](const StructDeclNode &s) {
                   declare(Symbol::type_sym(std::string(s.name.name), nullptr,
                                            s.name.span, s.is_public));
                 },
                 [&](const EnumDeclNode &e) {
                   declare(Symbol::type_sym(std::string(e.name.name), nullptr,
                                            e.name.span, e.is_public));
                 },
                 [&](const InterfaceDeclNode &i) {
                   declare(Symbol::type_sym(std::string(i.name.name), nullptr,
                                            i.name.span, i.is_public));
                 },
                 [&](const ConstDeclNode &c) {
                   declare(Symbol::constant(std::string(c.name.name), nullptr,
                                            c.name.span, c.is_public));
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
    return builtins.error_type;
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
Analyzer::load_import_from_sgi(const std::string &import_path) {
  if (!package_resolver)
    return std::nullopt;

  std::string sgi_path = package_resolver->find_sgi_file(import_path);
  if (sgi_path.empty())
    return std::nullopt;

  auto sgi = load_sgi(sgi_path);
  if (!sgi)
    return std::nullopt;

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
    return builtins.error_type;
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
    return builtins.error_type;
  }

  auto source_files = package_resolver->list_source_files(pkg_dir);
  if (source_files.empty()) {
    error(span,
          std::format("package '{}' contains no source files", import_path));
    return builtins.error_type;
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
    return builtins.error_type;
  }

  Parser sub_parser(sub_fileset);
  auto sub_ast = sub_parser.parse();
  if (!sub_ast || !sub_parser.errors.errors.empty()) {
    error(span,
          std::format("parse errors in imported package '{}'", import_path));
    package_resolver->in_progress.erase(import_path);
    return builtins.error_type;
  }

  Analyzer sub_analyzer(sub_fileset, package_resolver);
  sub_analyzer.current_package_dir = pkg_dir;
  sub_analyzer.analyze(*sub_ast);

  if (!sub_analyzer.errors.errors.empty()) {
    error(span, std::format("errors in imported package '{}'", import_path));
    for (auto &e : sub_analyzer.errors.errors)
      errors.errors.push_back(e);
    package_resolver->in_progress.erase(import_path);
    return builtins.error_type;
  }

  merge_sub_analyzer_receiver_methods(sub_analyzer);

  auto last_slash = import_path.rfind('/');
  std::string pkg_name = (last_slash != std::string::npos)
                             ? import_path.substr(last_slash + 1)
                             : import_path;
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
  if (auto from_sgi = load_import_from_sgi(import_path))
    return *from_sgi;
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

  loaded_source_packages_.emplace(
      origin,
      LoadedSourcePackage{std::move(loaded_fileset), std::move(sub_ast),
                          std::move(sub_analyzer)});
  return sub_ptr;
}

Analyzer::ImportedMethodDecl
Analyzer::load_imported_method_decl(const std::string &origin,
                                     const std::string &struct_name,
                                     const std::string &method_name,
                                     const std::vector<TypePtr> &type_args) {
  ImportedMethodDecl result;
  auto *sub = ensure_source_loaded(origin);
  if (!sub || !sub->package_scope_) return result;

  auto sym_it = sub->package_scope_->symbols.find(struct_name);
  if (sym_it == sub->package_scope_->symbols.end()) return result;
  auto struct_type = sym_it->second.type;
  if (!struct_type || struct_type->kind != TypeKind::Struct) return result;

  auto &sinfo = std::get<StructTypeInfo>(struct_type->detail);
  for (auto &m : sinfo.methods) {
    if (m.name != method_name || !m.signature) continue;
    auto fd_it = sub->func_decl_by_type_.find(m.signature.get());
    if (fd_it == sub->func_decl_by_type_.end()) return result;
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
  auto tpl_it = sub->generic_templates_.find(result.decl);
  if (tpl_it != sub->generic_templates_.end()) {
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
        sub->instantiate_generic_body(*result.decl, result.bindings,
                                       *result.decl->body);
  }
  return result;
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
          [&](const RangeTypeNode &n) -> TypePtr {
            return resolve_range_type(n);
          },
          [&](const StructTypeNode &n) -> TypePtr {
            return resolve_struct_type(n);
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
            return builtins.error_type;
          },
      },
      node.data);
}

TypePtr Analyzer::resolve_identifier_type(const IdentifierNode &node) {
  auto sym = lookup(std::string(node.name));
  if (!sym) {
    undefined_error(node.span, std::string(node.name));
    return builtins.error_type;
  }
  if (sym->kind != SymbolKind::Type && sym->kind != SymbolKind::TypeParam) {
    error(node.span, std::format("'{}' is not a type", std::string(node.name)));
    return builtins.error_type;
  }
  return sym->type ? sym->type : builtins.error_type;
}

TypePtr Analyzer::resolve_selector_type(const SelectorNode &node) {
  auto *obj_ident = std::get_if<IdentifierNode>(&node.object->data);
  if (!obj_ident) {
    error(node.span, "expected package name in qualified type");
    return builtins.error_type;
  }
  auto mod_sym = lookup(std::string(obj_ident->name));
  if (!mod_sym || !mod_sym->type ||
      mod_sym->type->kind != TypeKind::Module) {
    error(obj_ident->span,
          std::format("'{}' is not a package", obj_ident->name));
    return builtins.error_type;
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
  return builtins.error_type;
}

TypePtr Analyzer::resolve_array_type(const ArrayTypeNode &node) {
  auto elem = resolve_type(*node.element_type);
  return make_array_type(std::move(elem));
}

TypePtr Analyzer::resolve_map_type(const MapTypeNode &node) {
  auto key = resolve_type(*node.key_type);
  auto val = resolve_type(*node.value_type);
  return make_map_type(std::move(key), std::move(val));
}

TypePtr Analyzer::resolve_func_type(const FuncTypeNode &node) {
  std::vector<TypePtr> params;
  for (auto &p : node.params)
    params.push_back(resolve_type(*p));
  std::vector<TypePtr> rets;
  for (auto &r : node.returns)
    rets.push_back(resolve_type(*r));
  return make_func_type(std::move(params), std::move(rets));
}

TypePtr Analyzer::resolve_range_type(const RangeTypeNode &node) {
  auto elem = resolve_type(*node.element_type);
  return make_range_type(std::move(elem));
}

TypePtr Analyzer::resolve_struct_type(const StructTypeNode &node) {
  std::vector<FieldInfo> fields;
  for (auto &fs : node.fields) {
    auto ft = resolve_type(*fs.type);
    for (auto &ident : fs.names.identifiers) {
      fields.push_back({std::string(ident.name), ft, false});
    }
  }
  return make_struct_type("<anonymous>", std::move(fields));
}

TypePtr Analyzer::resolve_union_type(const UnionTypeNode &node) {
  std::vector<TypePtr> alts;
  for (auto &t : node.types)
    alts.push_back(resolve_type(*t));
  return make_union_type(std::move(alts));
}

TypePtr
Analyzer::resolve_generic_type_app(const GenericTypeAppNode &node) {
  auto base = resolve_type(*node.base_type);
  if (is_error_type(base))
    return builtins.error_type;

  // Resolve concrete type arguments.
  std::vector<TypePtr> args;
  for (auto &ta : node.type_args)
    args.push_back(resolve_type(*ta));

  // The base must be a generic struct (Task, etc.).
  if (base->kind != TypeKind::Struct) {
    error(node.span,
          std::format("type {} is not generic", type_to_string(base)));
    return builtins.error_type;
  }

  auto &info = std::get<StructTypeInfo>(base->detail);
  if (info.type_params.empty()) {
    error(node.span,
          std::format("type {} is not generic", type_to_string(base)));
    return builtins.error_type;
  }

  if (args.size() != info.type_params.size()) {
    error(node.span,
          std::format("expected {} type argument(s), got {}",
                      info.type_params.size(), args.size()));
    return builtins.error_type;
  }

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
    for (size_t i = 0; i < p.names.identifiers.size(); ++i) {
      if (p.is_variadic && i == p.names.identifiers.size() - 1) {
        // Variadic wraps the type in an array.
        params.push_back(make_array_type(pt));
      } else {
        params.push_back(pt);
      }
    }
  }
  std::vector<TypePtr> returns;
  for (auto &r : sig.returns)
    returns.push_back(resolve_type(*r));
  return make_func_type(std::move(params), std::move(returns));
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

void Analyzer::resolve_declaration(const Node &node) {
  std::visit(overloaded{
                 [&](const FuncDeclNode &fn) { resolve_func_decl(fn); },
                 [&](const StructDeclNode &s) { resolve_struct_decl(s); },
                 [&](const EnumDeclNode &e) { resolve_enum_decl(e); },
                 [&](const InterfaceDeclNode &i) { resolve_interface_decl(i); },
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
          if (check_recv_origin(struct_info.origin_package, struct_info.name))
            struct_info.methods.push_back(
                {std::string(fn.name.name), fn_type, fn.is_public,
                 current_package_name()});
        } else if (recv_sym->type->kind == TypeKind::Alias) {
          auto &alias_info = std::get<AliasTypeInfo>(recv_sym->type->detail);
          alias_info.methods.push_back(
              {std::string(fn.name.name), fn_type, fn.is_public,
               current_package_name()});
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
        if (!dup)
          vec.push_back({std::string(fn.name.name), normalized, fn.is_public});
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
        if (!dup)
          vec.push_back({std::string(fn.name.name), normalized, fn.is_public});
      }
    }
  }

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
  if (fn_type && (!fn.receiver || is_generic_on_concrete_recv)) {
    func_decl_by_type_[fn_type.get()] = &fn;
  }

  // Stash the generic template for lazy body-analysis at instantiation
  // time.  Generic receiver methods on concrete types are included so
  // their bodies are checked per-instantiation (same as free generics).
  if (has_generics && (!fn.receiver || is_generic_on_concrete_recv)) {
    generic_templates_[&fn] =
        GenericTemplate{&fn, current_scope, std::move(generic_params)};
  }
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

  // Resolve fields and methods.
  for (auto &member : s.members) {
    std::visit(overloaded{
                   [&](const FieldSpecNode &fs) {
                     auto ft = resolve_type(*fs.type);
                     for (auto &ident : fs.names.identifiers) {
                       fields.push_back(
                           {std::string(ident.name), ft, member.is_public});
                     }
                   },
                   [&](const FuncDeclNode &fn) {
                     // Shadowing check: method-level type params must not
                     // reuse names from the enclosing struct's type params.
                     if (fn.generic && !type_params.empty()) {
                       for (auto &tp_node : fn.generic->type_params) {
                         auto &tp_ident =
                             std::get<IdentifierNode>(tp_node->data);
                         for (auto &stp : type_params) {
                           if (stp.name == std::string(tp_ident.name)) {
                             error(tp_node->span,
                                   std::format("type parameter '{}' shadows "
                                               "enclosing struct type "
                                               "parameter",
                                               tp_ident.name));
                           }
                         }
                       }
                     }
                     auto fn_type = resolve_signature(fn.signature);
                     methods.push_back({std::string(fn.name.name), fn_type,
                                        member.is_public,
                                        current_package_name()});
                   },
                   [&](const auto &) {},
               },
               member.member->data);
  }

  // Resolve embeds. Each entry is an IdentifierNode (local) or a
  // SelectorNode (`pkg.Name`); resolve_type handles both, so we get
  // qualified-name support for free.
  std::vector<TypePtr> embeds;
  for (auto &embed_node : s.embeds) {
    auto et = resolve_type(*embed_node);
    if (!et || et->kind == TypeKind::Error) continue;
    if (et->kind != TypeKind::Struct) {
      error(embed_node->span, "embedded type must be a struct");
      continue;
    }
    embeds.push_back(et);
  }

  auto struct_type =
      make_struct_type(std::string(s.name.name), std::move(fields),
                       std::move(methods), std::move(type_params),
                       current_package_name());
  // Set embeds on the created type.
  auto &info = std::get<StructTypeInfo>(struct_type->detail);
  info.embeds = std::move(embeds);

  // Update the symbol.
  auto sym_it = current_scope->symbols.find(std::string(s.name.name));
  // If we pushed a scope for generics, the symbol is in the parent.
  auto &target_scope = has_generics ? current_scope->parent : current_scope;
  sym_it = target_scope->symbols.find(std::string(s.name.name));
  if (sym_it != target_scope->symbols.end()) {
    sym_it->second.type = struct_type;
  }

  if (has_generics) {
    pop_scope();
  }
}

void Analyzer::resolve_enum_decl(const EnumDeclNode &e) {
  std::vector<EnumVariant> variants;
  int64_t next_index = 0;
  for (auto &field : e.fields) {
    std::vector<FieldInfo> variant_fields;
    int64_t explicit_index = -1;
    for (auto &fa : field.initializer) {
      auto field_type = check_expr(*fa.value);
      variant_fields.push_back({std::string(fa.name.name), field_type, false});
      // Extract explicit index value.
      if (std::string(fa.name.name) == "index") {
        if (auto *lit = std::get_if<IntegerLiteralNode>(&fa.value->data)) {
          std::string clean;
          for (char c : lit->literal)
            if (c != '_') clean += c;
          explicit_index = std::stoll(clean);
        }
      }
    }
    if (explicit_index >= 0)
      next_index = explicit_index;
    variants.push_back(
        {std::string(field.name.name), std::move(variant_fields), next_index});
    next_index++;
  }

  auto enum_type =
      make_enum_type(std::string(e.name.name), std::move(variants),
                     current_package_name());

  auto sym_it = current_scope->symbols.find(std::string(e.name.name));
  if (sym_it != current_scope->symbols.end()) {
    sym_it->second.type = enum_type;
  }
}

void Analyzer::resolve_interface_decl(const InterfaceDeclNode &i) {
  std::vector<MethodInfo> methods;
  std::vector<TypeParam> type_params;

  bool has_generics = i.generic.has_value();
  if (has_generics) {
    push_scope(ScopeKind::Block);
    type_params = enter_generics(*i.generic);
  }

  for (auto &field : i.methods) {
    auto fn_type = resolve_signature(field.signature);
    methods.push_back({std::string(field.name.name), fn_type, field.is_public,
                       current_package_name()});
  }

  auto iface_type = make_interface_type(
      std::string(i.name.name), std::move(methods), std::move(type_params),
      current_package_name());

  auto &target_scope = has_generics ? current_scope->parent : current_scope;
  auto sym_it = target_scope->symbols.find(std::string(i.name.name));
  if (sym_it != target_scope->symbols.end()) {
    sym_it->second.type = iface_type;
  }

  if (has_generics) {
    pop_scope();
  }
}

void Analyzer::resolve_const_decl(const ConstDeclNode &c) {
  TypePtr const_type = nullptr;
  if (c.type) {
    const_type = resolve_type(**c.type);
  }

  // Detect type alias pattern: const Name = TypeIdentifier
  // If the value is an identifier (or selector) that refers to a type,
  // create an alias type so methods can be bound to it.
  if (!const_type && c.value) {
    TypePtr alias_underlying = nullptr;
    if (auto *ident = std::get_if<IdentifierNode>(&c.value->data)) {
      auto sym = lookup(std::string(ident->name));
      if (sym && sym->kind == SymbolKind::Type && sym->type) {
        alias_underlying = sym->type;
      }
    } else if (auto *sel = std::get_if<SelectorNode>(&c.value->data)) {
      // Handle math.Point style type aliases.
      if (auto *obj_ident = std::get_if<IdentifierNode>(&sel->object->data)) {
        auto obj_sym = lookup(std::string(obj_ident->name));
        if (obj_sym && obj_sym->kind == SymbolKind::Module && obj_sym->type &&
            obj_sym->type->kind == TypeKind::Module) {
          auto &mod = std::get<ModuleTypeInfo>(obj_sym->type->detail);
          std::string field_name(sel->field.name);
          for (auto &exp : mod.exports) {
            if (exp.name == field_name && exp.type) {
              // Check if the export is a type (struct, enum, interface, alias).
              if (exp.type->kind == TypeKind::Struct ||
                  exp.type->kind == TypeKind::Enum ||
                  exp.type->kind == TypeKind::Interface ||
                  exp.type->kind == TypeKind::Alias) {
                alias_underlying = exp.type;
              }
              break;
            }
          }
        }
      }
    }

    if (alias_underlying) {
      // Create a unique alias type that inherits the underlying type's methods.
      auto alias_type = make_alias_type(
          std::string(c.name.name), alias_underlying, {},
          current_package_name());
      auto sym_it = current_scope->symbols.find(std::string(c.name.name));
      if (sym_it != current_scope->symbols.end()) {
        sym_it->second.type = alias_type;
        sym_it->second.kind = SymbolKind::Type;
      }
      return;
    }
  }

  // The initializer expression will be type-checked later; for now we just
  // record the declared type if present.
  auto sym_it = current_scope->symbols.find(std::string(c.name.name));
  if (sym_it != current_scope->symbols.end() && const_type) {
    sym_it->second.type = const_type;
  }
}

// ===========================================================================
// Phase 3 — Name resolution in function/method bodies
// ===========================================================================

// Helper: resolve names inside a function declaration body.  Called from
// visit_source after all declarations have been resolved.
void Analyzer::inject_struct_fields(const TypePtr &struct_type) {
  if (!struct_type || struct_type->kind != TypeKind::Struct)
    return;
  auto &info = std::get<StructTypeInfo>(struct_type->detail);
  for (auto &field : info.fields) {
    // Inject as a variable so the field name resolves in scope.
    // Use declare() (not declare_local) to avoid shadowing errors
    // against the outer struct scope.
    current_scope->symbols.emplace(
        field.name, Symbol::variable(field.name, field.type, Span{}));
  }
}

void Analyzer::resolve_func_decl_body(const FuncDeclNode &fn,
                                      const TypePtr &enclosing_struct) {
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

  // For in-bound methods, inject the enclosing struct's fields into scope.
  if (enclosing_struct) {
    inject_struct_fields(enclosing_struct);
  }

  // Declare the receiver if present.
  if (fn.receiver) {
    auto recv_type = resolve_type(*fn.receiver->type);
    declare_local(Symbol::parameter(std::string(fn.receiver->name.name),
                                    recv_type, fn.receiver->name.span));
  }

  // Set return types on the function scope.
  for (auto &r : fn.signature.returns) {
    current_scope->return_types.push_back(resolve_type(*r));
  }

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
          [&](const IntegerLiteralNode &) { /* leaf */ },
          [&](const FloatLiteralNode &) { /* leaf */ },
          [&](const StringLiteralNode &n) { resolve_string_literal(n); },
          [&](const StringFragmentNode &) { /* leaf */ },
          [&](const ArrayLiteralNode &n) { resolve_array_literal(n); },
          [&](const MapLiteralNode &n) { resolve_map_literal(n); },
          [&](const StructLiteralNode &n) { resolve_struct_literal(n); },
          [&](const BinaryExprNode &n) { resolve_binary_expr(n); },
          [&](const UnaryExprNode &n) { resolve_unary_expr(n); },
          [&](const GroupExprNode &n) { resolve_group_expr(n); },
          [&](const CallExprNode &n) { resolve_call_expr(n); },
          [&](const IndexExprNode &n) { resolve_index_expr(n); },
          [&](const SelectorNode &n) { resolve_selector(n); },
          [&](const IfExprNode &n) { resolve_if_expr(n); },
          [&](const SwitchExprNode &n) { resolve_switch_expr(n); },
          [&](const ForExprNode &n) { resolve_for_expr(n); },
          [&](const RangeExprNode &n) { resolve_range_expr(n); },
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
    resolve_expr(*arm.pattern);
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

void Analyzer::resolve_range_expr(const RangeExprNode &node) {
  resolve_expr(*node.low);
  resolve_expr(*node.high);
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
                                   builtins.error_iface, node.pipe->span));
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
  for (auto &r : node.signature.returns) {
    current_scope->return_types.push_back(resolve_type(*r));
  }

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
  for (auto &val : node.values) {
    resolve_expr(*val);
  }
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

void Analyzer::check_func_decl_body(const FuncDeclNode &fn,
                                    const TypePtr &enclosing_struct) {
  // Generic functions are type-checked lazily per instantiation.
  // Receiver methods on generic receiver types (Array/Map) still flow
  // through the eager path because their T is the element type.
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

  if (fn.generic)
    enter_generics(*fn.generic);

  // For in-bound methods, inject the enclosing struct's fields into scope.
  if (enclosing_struct) {
    inject_struct_fields(enclosing_struct);
  }

  if (fn.receiver) {
    auto recv_type = resolve_type(*fn.receiver->type);
    declare_local(Symbol::parameter(std::string(fn.receiver->name.name),
                                    recv_type, fn.receiver->name.span));
  }

  for (auto &r : fn.signature.returns)
    current_scope->return_types.push_back(resolve_type(*r));

  declare_parameters(fn.signature);

  auto &block = std::get<BlockNode>(fn.body->data);
  auto body_type = check_block(block);

  // Check that the tail expression matches the return type (if non-Void).
  // If the last statement always returns (directly or through branches),
  // the return values were already checked by check_return.
  if (!current_scope->return_types.empty() && !block.stmts.empty()) {
    bool tail_is_return = always_returns(*block.stmts.back());
    if (!tail_is_return) {
      auto &expected = current_scope->return_types;
      if (expected.size() == 1 && !is_error_type(body_type)) {
        if (!types_equal(expected[0], builtins.void_type)) {
          expect_assignable(fn.body->span, expected[0], body_type,
                            "return type");
        }
      }
    }
  }

  pop_scope();
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
          [&](const RangeExprNode &n) -> TypePtr {
            return check_range_expr(n);
          },
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
          [&](const auto &) -> TypePtr { return builtins.error_type; },
      },
      node.data);

  record_type(node, type);
  return type;
}

TypePtr Analyzer::check_identifier(const IdentifierNode &ident,
                                   const Node &parent) {
  std::string name(ident.name);
  if (!name.empty() && name[0] == '_')
    return builtins.void_type;

  auto sym = lookup(name);
  if (!sym) {
    // Already reported during name resolution.
    return builtins.error_type;
  }
  record_symbol(parent, *sym);

  // For module symbols, return the module type directly.
  if (sym->kind == SymbolKind::Module && sym->type)
    return sym->type;

  return sym->type ? sym->type : builtins.error_type;
}

TypePtr Analyzer::check_bool_literal(const BoolLiteralNode &) {
  return builtins.bool_type;
}

TypePtr Analyzer::check_int_literal(const IntegerLiteralNode &) {
  return builtins.int_type;
}

TypePtr Analyzer::check_float_literal(const FloatLiteralNode &) {
  return builtins.float_type;
}

TypePtr Analyzer::check_string_literal(const StringLiteralNode &node) {
  // Verify all interpolated expressions implement Stringer (have .String()).
  // For now, just check all fragments; a deeper Stringer check is deferred.
  for (auto &frag : node.fragments) {
    if (!std::holds_alternative<StringFragmentNode>(frag->data)) {
      check_expr(*frag);
    }
  }
  return builtins.string_type;
}

TypePtr Analyzer::check_array_literal(const ArrayLiteralNode &node) {
  if (node.elements.empty()) {
    // Empty array — type must be inferred from context.  Return a
    // placeholder; the assignment checker will fill it in.
    return make_array_type(builtins.error_type);
  }
  auto elem_type = check_expr(*node.elements[0]);
  for (size_t i = 1; i < node.elements.size(); ++i) {
    auto t = check_expr(*node.elements[i]);
    if (!is_error_type(t) && !is_error_type(elem_type)) {
      expect_assignable(node.elements[i]->span, elem_type, t, "array element");
    }
  }
  return make_array_type(elem_type);
}

TypePtr Analyzer::check_map_literal(const MapLiteralNode &node) {
  if (node.entries.empty()) {
    return make_map_type(builtins.error_type, builtins.error_type);
  }
  auto key_type = check_expr(*node.entries[0].key);
  auto val_type = check_expr(*node.entries[0].value);
  for (size_t i = 1; i < node.entries.size(); ++i) {
    auto kt = check_expr(*node.entries[i].key);
    auto vt = check_expr(*node.entries[i].value);
    if (!is_error_type(kt))
      expect_assignable(node.entries[i].key->span, key_type, kt, "map key");
    if (!is_error_type(vt))
      expect_assignable(node.entries[i].value->span, val_type, vt, "map value");
  }
  return make_map_type(key_type, val_type);
}

TypePtr Analyzer::check_struct_literal(const StructLiteralNode &node) {
  auto type_expr_type = check_expr(*node.type_expr);
  if (is_error_type(type_expr_type))
    return builtins.error_type;

  // For alias types, unwrap to get the underlying struct type for validation,
  // but return the alias type so the variable retains its alias identity.
  auto struct_type = type_expr_type;
  if (struct_type->kind == TypeKind::Alias) {
    struct_type = unwrap_alias(struct_type);
  }

  if (!struct_type || struct_type->kind != TypeKind::Struct) {
    error(node.type_expr->span, std::format("'{}' is not a struct type",
                                            type_to_string(type_expr_type)));
    return builtins.error_type;
  }

  // Check each field value first and collect types for generic inference.
  std::vector<std::pair<std::string, TypePtr>> field_vals;
  for (auto &fa : node.fields) {
    auto val_type = check_expr(*fa.value);
    field_vals.push_back({std::string(fa.name.name), val_type});
  }

  // If the struct is generic, instantiate it by inferring type params.
  auto &raw_info = std::get<StructTypeInfo>(struct_type->detail);
  auto effective_type = struct_type;
  if (!raw_info.type_params.empty()) {
    effective_type =
        instantiate_generic_struct(struct_type, field_vals, node.span);
    if (is_error_type(effective_type))
      return builtins.error_type;
  }

  auto &info = std::get<StructTypeInfo>(effective_type->detail);

  // Collect all fields including those from embedded types.
  auto all_fields = info.fields;
  for (auto &embed : info.embeds) {
    if (embed && embed->kind == TypeKind::Struct) {
      auto &embed_info = std::get<StructTypeInfo>(embed->detail);
      all_fields.insert(all_fields.end(), embed_info.fields.begin(),
                        embed_info.fields.end());
    }
  }

  // Validate each field assignment against the (possibly instantiated) type.
  for (size_t i = 0; i < field_vals.size(); ++i) {
    auto &[fname, val_type] = field_vals[i];
    bool found = false;
    for (auto &fi : all_fields) {
      if (fi.name == fname) {
        found = true;
        if (fi.type && !is_error_type(val_type)) {
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
    if (!found) {
      error(node.span,
            std::format("struct '{}' has no field '{}'", info.name, fname));
    }
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
    return builtins.error_type;

  case K::Sub:
    if (has_method("Sub")) {
      expect_assignable(node.rhs->span, lhs, rhs, "Sub argument");
      return resolve("Sub", lhs);
    }
    error(node.span,
          std::format("type {} does not implement Subber (no Sub method)",
                      type_to_string(lhs)));
    return builtins.error_type;

  // ── Multiplicative ───────────────────────────────────────────────────────
  case K::Multiply:
    if (has_method("Mul")) {
      expect_assignable(node.rhs->span, lhs, rhs, "Mul argument");
      return resolve("Mul", lhs);
    }
    error(node.span,
          std::format("type {} does not implement Multiplier (no Mul method)",
                      type_to_string(lhs)));
    return builtins.error_type;

  case K::Divide:
    if (has_method("Div")) {
      expect_assignable(node.rhs->span, lhs, rhs, "Div argument");
      // Divisable returns T | Error (can fail, e.g. divide by zero).
      return resolve("Div", make_union_type({lhs, builtins.error_iface}));
    }
    error(node.span,
          std::format("type {} does not implement Divisable (no Div method)",
                      type_to_string(lhs)));
    return builtins.error_type;

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
    return builtins.error_type;

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
    return builtins.error_type;

  default:
    error(node.span,
          std::format("operator not supported for type {}",
                      type_to_string(lhs)));
    return builtins.error_type;
  }
}

TypePtr Analyzer::check_binary_expr(const BinaryExprNode &node,
                                    const Node &parent) {
  auto lhs = check_expr(*node.lhs);
  auto rhs = check_expr(*node.rhs);

  if (is_error_type(lhs) || is_error_type(rhs))
    return builtins.error_type;

  // ── Struct operator overloading ──────────────────────────────────────────
  // Dispatch to method-based overloading before the built-in numeric/string
  // paths, so user types can override operators on structs.
  // Exception: Any is the stdlib top/bottom type — skip method dispatch and
  // fall through to the built-in operator paths below.
  auto is_any_type = [](const TypePtr &t) {
    if (!t || t->kind != TypeKind::Struct) return false;
    return std::get<StructTypeInfo>(t->detail).name == "Any";
  };
  if (lhs->kind == TypeKind::Struct && !is_any_type(lhs)) {
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
      return builtins.error_type;
    }
    if (!is_numeric(rhs)) {
      error(node.rhs->span,
            std::format("arithmetic operator requires numeric type, got {}",
                        type_to_string(rhs)));
      return builtins.error_type;
    }
    return common_type(lhs, rhs);
  }

  // Division: returns T | Error (division by zero).
  case K::Divide: {
    if (!is_numeric(lhs) || !is_numeric(rhs)) {
      error(node.span,
            std::format("division requires numeric types, got {} and {}",
                        type_to_string(lhs), type_to_string(rhs)));
      return builtins.error_type;
    }
    auto result = common_type(lhs, rhs);
    return make_union_type({result, builtins.error_iface});
  }

  // Comparison: == != > < >= <=
  case K::Equal:
  case K::NotEqual: {
    // Type matching: if LHS is a union and RHS is a type name, this is a
    // type assertion (e.g. `value == Int`). Returns Bool.
    if (lhs->kind == TypeKind::Union) {
      // Check if RHS is a type symbol.
      if (auto *rhs_ident = std::get_if<IdentifierNode>(&node.rhs->data)) {
        auto sym = lookup(std::string(rhs_ident->name));
        if (sym && sym->kind == SymbolKind::Type) {
          // Verify the type is one of the union alternatives.
          auto &union_info = std::get<UnionTypeInfo>(lhs->detail);
          bool found = false;
          for (auto &alt : union_info.alternatives) {
            if (types_equal(alt, rhs) || is_assignable_to(rhs, alt))
              found = true;
          }
          if (!found) {
            error(node.rhs->span,
                  std::format("type {} is not an alternative of {}",
                              type_to_string(rhs), type_to_string(lhs)));
          }
          return builtins.bool_type;
        }
      }
    }
    if (!is_equatable(lhs)) {
      error(node.lhs->span, std::format("type {} does not support equality",
                                        type_to_string(lhs)));
      return builtins.error_type;
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
      return builtins.error_type;
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
      return builtins.error_type;
    }
    if (rhs->kind != TypeKind::Int) {
      error(node.rhs->span,
            std::format("bitwise operator requires integer type, got {}",
                        type_to_string(rhs)));
      return builtins.error_type;
    }
    return common_type(lhs, rhs);
  }

  default:
    error(node.span, "unsupported binary operator");
    return builtins.error_type;
  }
}

TypePtr Analyzer::check_unary_expr(const UnaryExprNode &node) {
  auto operand = check_expr(*node.operand);
  if (is_error_type(operand))
    return builtins.error_type;

  if (node.op == Token::Kind::Not) {
    expect_bool(node.operand->span, operand, "logical not");
    return builtins.bool_type;
  }
  if (node.op == Token::Kind::Sub) {
    if (!is_numeric(operand)) {
      error(node.operand->span,
            std::format("negation requires numeric type, got {}",
                        type_to_string(operand)));
      return builtins.error_type;
    }
    return operand;
  }
  if (node.op == Token::Kind::BitwiseNot) {
    if (operand->kind != TypeKind::Int) {
      error(node.operand->span,
            std::format("bitwise NOT requires integer type, got {}",
                        type_to_string(operand)));
      return builtins.error_type;
    }
    return operand;
  }

  error(node.span, "unsupported unary operator");
  return builtins.error_type;
}

TypePtr Analyzer::check_group_expr(const GroupExprNode &node) {
  return check_expr(*node.inner);
}

TypePtr Analyzer::check_call_expr(const CallExprNode &node,
                                  const Node &parent) {
  // Gate all intrinsic_* calls to stdlib packages only.
  if (auto *ident = std::get_if<IdentifierNode>(&node.callee->data)) {
    if (ident->name.starts_with("intrinsic_") && !is_stdlib) {
      error(node.callee->span,
            std::format("'{}' can only be called from stdlib packages",
                        ident->name));
      return builtins.error_type;
    }
  }

  auto callee_type = check_expr(*node.callee);
  if (is_error_type(callee_type))
    return builtins.error_type;

  if (!is_callable(callee_type)) {
    error(node.callee->span,
          std::format("'{}' is not callable", type_to_string(callee_type)));
    return builtins.error_type;
  }

  // Check arguments first to collect their types.
  std::vector<TypePtr> arg_types;
  for (auto &arg : node.args) {
    arg_types.push_back(check_expr(*arg));
  }

  // If the callee contains type parameters, attempt generic instantiation.
  auto effective_type = callee_type;
  if (has_type_params(callee_type)) {
    std::unordered_map<uint32_t, TypePtr> bindings;
    auto instantiated =
        instantiate_generic_call(callee_type, arg_types, node.span, &bindings);
    if (!is_error_type(instantiated))
      effective_type = instantiated;

    // For generic free functions, analyse the body with these concrete
    // bindings so member-access, operator-overloading and capture
    // tracking see concrete types.  Methods on generic types (Array/Map)
    // and receiver-method calls go through their own paths and aren't
    // registered in generic_templates_.
    if (!bindings.empty() && callee_type) {
      auto fd_it = func_decl_by_type_.find(callee_type.get());
      if (fd_it != func_decl_by_type_.end() &&
          generic_templates_.find(fd_it->second) != generic_templates_.end()) {
        instantiate_generic_body(*fd_it->second, bindings, parent);
        if (current_instantiation_) {
          current_instantiation_->node_type_args[&parent] = bindings;
        } else {
          node_type_args[&parent] = bindings;
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
      return builtins.error_type;
    }
  } else {
    if (fn_info.params.size() > 0 &&
        arg_types.size() < fn_info.params.size() - 1) {
      error(node.span,
            std::format("expected at least {} argument(s), got {}",
                        fn_info.params.size() - 1, arg_types.size()));
      return builtins.error_type;
    }
  }

  // Check argument types against the (possibly instantiated) signature.
  for (size_t i = 0; i < arg_types.size(); ++i) {
    bool is_variadic_param = fn_info.is_variadic && !fn_info.params.empty() &&
                             i >= fn_info.params.size() - 1;
    if (is_variadic_param) {
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
  if (fn_info.returns.empty())
    return builtins.void_type;
  if (fn_info.returns.size() == 1)
    return fn_info.returns[0];
  return builtins.void_type;
}

TypePtr Analyzer::check_index_expr(const IndexExprNode &node) {
  auto obj_type = check_expr(*node.object);
  if (is_error_type(obj_type))
    return builtins.error_type;

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
    return builtins.error_type;
  }

  auto index_type = check_expr(*node.index);

  switch (obj_type->kind) {
  case TypeKind::Array: {
    auto &arr = std::get<ArrayTypeInfo>(obj_type->detail);
    if (!is_error_type(index_type) && index_type->kind != TypeKind::Int) {
      error(node.index->span, "array index must be an integer");
    }
    // Indexing returns T | Error (out of bounds).
    return make_union_type({arr.element, builtins.error_iface});
  }
  case TypeKind::Map: {
    auto &map_info = std::get<MapTypeInfo>(obj_type->detail);
    if (!is_error_type(index_type)) {
      expect_assignable(node.index->span, map_info.key, index_type, "map key");
    }
    // Map access returns V | Error (missing key).
    return make_union_type({map_info.value, builtins.error_iface});
  }
  case TypeKind::String: {
    if (!is_error_type(index_type) && index_type->kind != TypeKind::Int) {
      error(node.index->span, "string index must be an integer");
    }
    return builtins.string_type;
  }
  default:
    error(node.span, std::format("type {} does not support indexing",
                                 type_to_string(obj_type)));
    return builtins.error_type;
  }
}

TypePtr Analyzer::resolve_module_selector(const ModuleTypeInfo &mod,
                                          const std::string &field_name,
                                          Span field_span) {
  for (auto &exp : mod.exports)
    if (exp.name == field_name)
      return exp.type ? exp.type : builtins.error_type;
  error(field_span,
        std::format("package '{}' has no exported member '{}'", mod.name,
                    field_name));
  return builtins.error_type;
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
      return f.type ? f.type : builtins.error_type;

  for (auto &m : info.methods) {
    if (m.name != field_name)
      continue;
    auto sig = m.signature ? m.signature : builtins.error_type;
    if (!info.origin_package.empty() &&
        info.origin_package != current_package_name() &&
        has_type_params(sig)) {
      error(field_span,
            std::format("cannot call generic method '{}' across packages "
                        "(D3: generic method bodies are not cross-package "
                        "in this version)",
                        field_name));
      return builtins.error_type;
    }
    return sig;
  }

  for (auto &embed : info.embeds) {
    if (!embed || embed->kind != TypeKind::Struct)
      continue;
    auto &einfo = std::get<StructTypeInfo>(embed->detail);
    for (auto &f : einfo.fields)
      if (f.name == field_name)
        return f.type ? f.type : builtins.error_type;
    for (auto &m : einfo.methods)
      if (m.name == field_name)
        return m.signature ? m.signature : builtins.error_type;
  }
  return nullptr;
}

TypePtr Analyzer::resolve_method_signature(const TypePtr &obj_type,
                                           const std::string &field_name) {
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
        return m.signature ? m.signature : builtins.error_type;

  auto effective_kind = underlying_kind(obj_type);
  auto effective_type = unwrap_alias(obj_type);

  auto kind_it = kind_methods_.find(effective_kind);
  if (kind_it != kind_methods_.end()) {
    for (auto &m : kind_it->second) {
      if (m.name != field_name)
        continue;
      if (!m.signature)
        return builtins.error_type;
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
      return builtins.error_type;
    if (has_type_params(m.signature))
      return substitute_kind_method(effective_kind, effective_type,
                                    m.signature);
    return m.signature;
  }
  return nullptr;
}

TypePtr Analyzer::check_selector(const SelectorNode &node,
                                 const Node & /*parent*/) {
  auto obj_type = check_expr(*node.object);
  if (is_error_type(obj_type))
    return builtins.error_type;

  std::string field_name(node.field.name);

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
        return m.signature ? m.signature : builtins.error_type;
  }

  if (obj_type->kind == TypeKind::Enum) {
    auto &info = std::get<EnumTypeInfo>(obj_type->detail);
    for (auto &v : info.variants)
      if (v.name == field_name)
        return obj_type;
  }

  if (obj_type->kind == TypeKind::Alias) {
    auto &alias_info = std::get<AliasTypeInfo>(obj_type->detail);
    for (auto &m : alias_info.methods)
      if (m.name == field_name)
        return m.signature ? m.signature : builtins.error_type;
    auto underlying = unwrap_alias(obj_type);
    if (underlying && underlying->kind == TypeKind::Struct)
      if (auto t =
              resolve_struct_member(underlying, field_name, node.field.span))
        return t;
  }

  if (auto sig = resolve_method_signature(obj_type, field_name))
    return sig;

  error(node.field.span, std::format("type {} has no member '{}'",
                                     type_to_string(obj_type), field_name));
  return builtins.error_type;
}

TypePtr Analyzer::check_if_expr(const IfExprNode &node) {
  auto cond_type = check_expr(*node.condition);
  expect_bool(node.condition->span, cond_type);

  // Detect type-matching pattern: `if value == TypeName`
  // to narrow the variable in the then-block.
  std::string narrowed_var;
  TypePtr narrowed_type = nullptr;
  TypePtr else_narrowed_type = nullptr;

  if (auto *binop = std::get_if<BinaryExprNode>(&node.condition->data)) {
    if (binop->op == Token::Kind::Equal) {
      if (auto *lhs_id = std::get_if<IdentifierNode>(&binop->lhs->data)) {
        if (auto *rhs_id = std::get_if<IdentifierNode>(&binop->rhs->data)) {
          auto sym = lookup(std::string(rhs_id->name));
          if (sym && sym->kind == SymbolKind::Type) {
            auto lhs_sym = lookup(std::string(lhs_id->name));
            if (lhs_sym && lhs_sym->type &&
                lhs_sym->type->kind == TypeKind::Union) {
              narrowed_var = std::string(lhs_id->name);
              narrowed_type = sym->type;
              // Compute the else narrowed type (union minus matched type).
              auto &info = std::get<UnionTypeInfo>(lhs_sym->type->detail);
              std::vector<TypePtr> remaining;
              for (auto &alt : info.alternatives) {
                if (!types_equal(alt, sym->type))
                  remaining.push_back(alt);
              }
              if (remaining.size() == 1)
                else_narrowed_type = remaining[0];
              else if (remaining.size() > 1)
                else_narrowed_type = make_union_type(std::move(remaining));
            }
          }
        }
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
    // Check if the first arm's pattern is a type name.
    if (!node.arms.empty()) {
      if (auto *pid =
              std::get_if<IdentifierNode>(&node.arms[0].pattern->data)) {
        auto sym = lookup(std::string(pid->name));
        if (sym && sym->kind == SymbolKind::Type)
          is_type_match = true;
      }
    }
  }

  for (auto &arm : node.arms) {
    auto pattern_type = check_expr(*arm.pattern);

    if (is_type_match) {
      // Type matching: verify the pattern type is an alternative of the union.
      if (!is_error_type(pattern_type) && !is_error_type(subject_type)) {
        auto &info = std::get<UnionTypeInfo>(subject_type->detail);
        bool found = false;
        for (auto &alt : info.alternatives) {
          if (types_equal(alt, pattern_type) ||
              is_assignable_to(pattern_type, alt))
            found = true;
        }
        if (!found) {
          error(arm.pattern->span,
                std::format("type {} is not an alternative of {}",
                            type_to_string(pattern_type),
                            type_to_string(subject_type)));
        }
      }
    } else {
      // Value matching: pattern must be same type as subject.
      if (!is_error_type(pattern_type) && !is_error_type(subject_type)) {
        expect_assignable(arm.pattern->span, subject_type, pattern_type,
                          "case pattern");
      }
    }

    TypePtr arm_type;
    if (auto *block = std::get_if<BlockNode>(&arm.body->data)) {
      push_scope(ScopeKind::Block);
      // For type matching, narrow the subject variable in the arm scope.
      if (is_type_match && !subject_var.empty() &&
          !is_error_type(pattern_type)) {
        current_scope->symbols[subject_var] =
            Symbol::variable(subject_var, pattern_type, arm.pattern->span);
      }
      arm_type = check_block(*block);
      pop_scope();
    } else {
      arm_type = check_expr(*arm.body);
    }

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
  }

  return result_type ? result_type : builtins.void_type;
}

TypePtr Analyzer::check_for_expr(const ForExprNode &node,
                                 TypePtr accumulator_hint) {
  push_scope(ScopeKind::Loop);

  if (node.mode) {
    std::visit(overloaded{
                   [&](const ForRangeClauseNode &range) {
                     auto iter_type = check_expr(*range.iterable);
                     // Infer loop variable types from the iterable.
                     TypePtr elem_type = builtins.error_type;
                     TypePtr key_type = builtins.int_type;

                     if (!is_error_type(iter_type)) {
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
                       case TypeKind::Range: {
                         auto &r = std::get<RangeTypeInfo>(iter_type->detail);
                         elem_type = r.element;
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
                               if (!fi.returns.empty()) {
                                 auto &ret = fi.returns[0];
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
                             elem_type = builtins.error_type;
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
                             if (fi.returns.size() != 1 ||
                                 fi.returns[0]->kind != TypeKind::Union)
                               break;
                             // Extract T from T | Error.
                             auto &ui = std::get<UnionTypeInfo>(
                                 fi.returns[0]->detail);
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

  pop_scope();
  return node.accumulator ? acc_type : builtins.void_type;
}

TypePtr Analyzer::check_range_expr(const RangeExprNode &node) {
  auto low = check_expr(*node.low);
  auto high = check_expr(*node.high);

  if (!is_error_type(low) && !is_numeric(low)) {
    error(node.low->span, std::format("range requires numeric type, got {}",
                                      type_to_string(low)));
  }
  if (!is_error_type(high) && !is_numeric(high)) {
    error(node.high->span, std::format("range requires numeric type, got {}",
                                       type_to_string(high)));
  }

  auto elem = common_type(low, high);
  return make_range_type(elem ? elem : builtins.int_type);
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

  // Instantiate Task<T> with the explicit generic argument if provided,
  // e.g. |Int| spawn { ... } → Task<Int>.
  if (node.generic && !node.generic->type_params.empty()) {
    auto chan_type = resolve_type(*node.generic->type_params[0]);
    if (chan_type && !is_error_type(chan_type)) {
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
  }
  return builtins.task_type;
}

TypePtr Analyzer::check_or_expr(const OrExprNode &node) {
  auto expr_type = check_expr(*node.expr);

  if (is_error_type(expr_type)) {
    // Still check the fallback block for internal errors.
    push_scope(ScopeKind::Block);
    if (node.pipe) {
      current_scope->symbols.emplace(
          std::string(node.pipe->name),
          Symbol::variable(std::string(node.pipe->name), builtins.error_iface,
                           node.pipe->span));
    }
    auto &block = std::get<BlockNode>(node.fallback->data);
    check_block(block);
    pop_scope();
    return builtins.error_type;
  }

  // Check that the expression is an impure type (contains Error).
  bool has_error = false;
  if (expr_type->kind == TypeKind::Union) {
    auto &info = std::get<UnionTypeInfo>(expr_type->detail);
    for (auto &alt : info.alternatives) {
      if (alt->kind == TypeKind::Interface &&
          std::get<InterfaceTypeInfo>(alt->detail).name == "Error") {
        has_error = true;
        break;
      }
      // Also check for concrete types that satisfy Error (e.g. Missing).
      if (satisfies_interface(alt, builtins.error_iface)) {
        has_error = true;
        break;
      }
    }
  }
  if (!has_error && expr_type->kind != TypeKind::Union) {
    // Non-union, non-error type — or is a no-op. Allow but pass through.
  }

  // The or-clause strips the Error from the union.
  push_scope(ScopeKind::Block);

  if (node.pipe) {
    current_scope->symbols.emplace(
        std::string(node.pipe->name),
        Symbol::variable(std::string(node.pipe->name), builtins.error_iface,
                         node.pipe->span));
  }

  auto &block = std::get<BlockNode>(node.fallback->data);
  auto fallback_type = check_block(block);

  pop_scope();

  // Strip Error/Missing from the union to get the purified type.
  if (expr_type->kind == TypeKind::Union) {
    auto &info = std::get<UnionTypeInfo>(expr_type->detail);
    std::vector<TypePtr> purified;
    for (auto &alt : info.alternatives) {
      // Strip the Error interface.
      if (alt->kind == TypeKind::Interface &&
          std::get<InterfaceTypeInfo>(alt->detail).name == "Error") {
        continue;
      }
      // Strip concrete types that satisfy Error (e.g. Missing).
      if (alt->kind == TypeKind::Struct &&
          satisfies_interface(alt, builtins.error_iface)) {
        continue;
      }
      purified.push_back(alt);
    }
    if (purified.empty())
      return fallback_type ? fallback_type : builtins.void_type;
    if (purified.size() == 1) {
      // Validate fallback type matches the purified type (if non-empty block).
      if (fallback_type && !is_error_type(fallback_type) &&
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

  for (auto &r : node.signature.returns)
    current_scope->return_types.push_back(resolve_type(*r));

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
  if (!tail_is_return && fn_info.returns.size() == 1 &&
      !is_error_type(body_type)) {
    if (!types_equal(fn_info.returns[0], builtins.void_type)) {
      expect_assignable(node.body->span, fn_info.returns[0], body_type,
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
  // If not found, the import was already reported as an error.
  return builtins.error_type;
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
    // When the initializer is a for-expression with an accumulator,
    // pass the declared type so the accumulator variable is typed.
    if (auto *for_node = std::get_if<ForExprNode>(&(*var.init)->data)) {
      init_type = check_for_expr(*for_node, declared_type);
      record_type(**var.init, init_type);
    } else {
      init_type = check_expr(**var.init);
    }
    if (declared_type && !is_error_type(init_type)) {
      expect_assignable((*var.init)->span, declared_type, init_type,
                        "variable initializer");
    }
    if (!final_type)
      final_type = init_type;
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

void Analyzer::check_decl_assign(const DeclAssignNode &decl) {
  auto rhs_type = check_expr(*decl.value);

  // ── Multi-return unpacking ───────────────────────────────────────────
  // If there are multiple targets and the RHS is a call to a function with
  // multiple returns, assign each target the corresponding return type.
  if (decl.targets.identifiers.size() > 1) {
    if (auto *call = std::get_if<CallExprNode>(&decl.value->data)) {
      auto callee_type = check_expr(*call->callee);
      if (!is_error_type(callee_type) && callee_type->kind == TypeKind::Func) {
        auto &fn_info = std::get<FuncTypeInfo>(callee_type->detail);
        if (fn_info.returns.size() > 1) {
          if (decl.targets.identifiers.size() != fn_info.returns.size()) {
            error(decl.span, std::format("expected {} receiver(s), got {}",
                                         fn_info.returns.size(),
                                         decl.targets.identifiers.size()));
          }
          size_t count =
              std::min(decl.targets.identifiers.size(), fn_info.returns.size());
          for (size_t i = 0; i < count; ++i) {
            std::string name(decl.targets.identifiers[i].name);
            auto sym_it = current_scope->symbols.find(name);
            if (sym_it != current_scope->symbols.end()) {
              sym_it->second.type = fn_info.returns[i];
            } else {
              current_scope->symbols.emplace(
                  name, Symbol::variable(name, fn_info.returns[i],
                                         decl.targets.identifiers[i].span));
            }
          }
          return;
        }
      }
    }
  }

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

void Analyzer::check_assign(const AssignNode &node) {
  // Check each target and value.
  for (size_t i = 0; i < node.targets.size(); ++i) {
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
        if (!is_error_type(val_type)) {
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
        if (!is_error_type(val_type)) {
          expect_assignable(node.values[i]->span, target_type, val_type,
                            "compound assignment");
        }
      }
    }
  }
}

void Analyzer::check_increment(const IncrementNode &node) {
  auto t = check_expr(*node.operand);
  if (!is_error_type(t) && t->kind != TypeKind::Int) {
    error(node.span, std::format("increment requires integer type, got {}",
                                 type_to_string(t)));
  }
}

void Analyzer::check_decrement(const DecrementNode &node) {
  auto t = check_expr(*node.operand);
  if (!is_error_type(t) && t->kind != TypeKind::Int) {
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
  if (node.values.empty()) {
    // Bare return — function must be Void or have no return types.
    if (!expected.empty() && !types_equal(expected[0], builtins.void_type)) {
      error(node.span, "missing return value");
    }
    return;
  }

  if (node.values.size() != expected.size()) {
    error(node.span, std::format("return has {} value(s), expected {}",
                                 node.values.size(), expected.size()));
    return;
  }

  for (size_t i = 0; i < node.values.size(); ++i) {
    auto val_type = check_expr(*node.values[i]);
    expect_assignable(node.values[i]->span, expected[i], val_type,
                      "return value");
  }
}

void Analyzer::check_break(const BreakNode &node) {
  if (!current_scope->is_inside(ScopeKind::Loop)) {
    error(node.span, "'break' outside of loop");
    return;
  }
  for (auto &val : node.values) {
    check_expr(*val);
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
  // If this const was already resolved as a type alias in Phase 2,
  // don't overwrite the alias type.
  auto sym_it = current_scope->symbols.find(std::string(c.name.name));
  if (sym_it != current_scope->symbols.end() &&
      sym_it->second.kind == SymbolKind::Type &&
      sym_it->second.type && sym_it->second.type->kind == TypeKind::Alias) {
    return;
  }

  TypePtr declared_type = nullptr;
  if (c.type)
    declared_type = resolve_type(**c.type);

  auto init_type = check_expr(*c.value);

  if (declared_type && !is_error_type(init_type)) {
    expect_assignable(c.value->span, declared_type, init_type,
                      "constant initializer");
  }

  // Update symbol type.
  if (sym_it != current_scope->symbols.end()) {
    sym_it->second.type = declared_type ? declared_type : init_type;
  }
}

void Analyzer::check_enum_decl(const EnumDeclNode &e) {
  auto sym = lookup(std::string(e.name.name));
  if (!sym || !sym->type)
    return;

  // Check enum field initializer expressions.
  for (auto &field : e.fields) {
    for (auto &fa : field.initializer) {
      check_expr(*fa.value);
    }
  }
}

void Analyzer::check_func_decl(const FuncDeclNode &fn) {
  // The body is checked via check_func_decl_body.
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

  // Check duplicate methods.
  std::unordered_map<std::string, bool> seen_methods;
  for (auto &m : info.methods) {
    if (seen_methods.count(m.name)) {
      error(s.span, std::format("duplicate method '{}' in struct '{}'", m.name,
                                info.name));
    }
    seen_methods[m.name] = true;
  }
}

void Analyzer::check_interface_decl(const InterfaceDeclNode &i) {
  auto sym = lookup(std::string(i.name.name));
  if (!sym || !sym->type || sym->type->kind != TypeKind::Interface)
    return;

  // Check duplicate methods.
  auto &info = std::get<InterfaceTypeInfo>(sym->type->detail);
  std::unordered_map<std::string, bool> seen;
  for (auto &m : info.methods) {
    if (seen.count(m.name)) {
      error(i.span, std::format("duplicate method '{}' in interface '{}'",
                                m.name, info.name));
    }
    seen[m.name] = true;
  }
}

void Analyzer::check_import_decl(const ImportDeclNode &node) {
  // Import declarations are fully processed during the import phase (1.5).
  // Here we just verify the module symbol was successfully resolved.
  std::string path(node.path);
  auto last_slash = path.rfind('/');
  std::string name =
      (last_slash != std::string::npos) ? path.substr(last_slash + 1) : path;
  auto sym = lookup(name);
  if (sym && sym->type && is_error_type(sym->type)) {
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
    return builtins.error_type;

  auto &fn_info = std::get<FuncTypeInfo>(callee_type->detail);
  std::unordered_map<uint32_t, TypePtr> bindings;

  // Attempt to unify each parameter type with the argument type.
  size_t count = std::min(fn_info.params.size(), arg_types.size());
  for (size_t i = 0; i < count; ++i) {
    if (!unify(fn_info.params[i], arg_types[i], bindings)) {
      error(call_span,
            std::format("cannot infer type parameter from argument {}", i + 1));
      return builtins.error_type;
    }
  }

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
  instantiation_stack_.push_back(&call_node);

  current_scope = tpl.decl_scope->child(ScopeKind::Block);

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

  // Substituted return types for return-stmt checking.
  for (auto &r : fn.signature.returns) {
    auto rt = resolve_type(*r);
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
      if (expected.size() == 1 && !is_error_type(body_type)) {
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
  instantiation_stack_.pop_back();

  inst.in_progress = false;
  return &inst;
}

TypePtr Analyzer::instantiate_generic_struct(
    const TypePtr &struct_type,
    const std::vector<std::pair<std::string, TypePtr>> &field_types,
    Span span) {
  if (!struct_type || struct_type->kind != TypeKind::Struct)
    return builtins.error_type;

  auto &info = std::get<StructTypeInfo>(struct_type->detail);
  if (info.type_params.empty())
    return struct_type; // Not generic.

  std::unordered_map<uint32_t, TypePtr> bindings;

  // Unify each provided field value type against the struct's field type.
  for (auto &[fname, ftype] : field_types) {
    for (auto &fi : info.fields) {
      if (fi.name == fname && fi.type) {
        if (!unify(fi.type, ftype, bindings)) {
          error(span, std::format("cannot infer type parameter from field '{}'",
                                  fname));
          return builtins.error_type;
        }
        break;
      }
    }
  }

  if (bindings.empty())
    return struct_type;

  // Build the substituted struct type.
  std::vector<FieldInfo> new_fields;
  for (auto &f : info.fields) {
    new_fields.push_back({f.name, substitute(f.type, bindings), f.is_public});
  }
  std::vector<MethodInfo> new_methods;
  for (auto &m : info.methods) {
    new_methods.push_back(
        {m.name, substitute(m.signature, bindings), m.is_public,
         m.origin_package});
  }

  auto result = make_struct_type(info.name, std::move(new_fields),
                                 std::move(new_methods), {},
                                 info.origin_package);
  auto &result_info = std::get<StructTypeInfo>(result->detail);
  result_info.type_params = info.type_params;
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

bool Analyzer::satisfies_interface(const TypePtr &concrete,
                                   const TypePtr &iface) {
  if (!concrete || !iface)
    return false;
  if (iface->kind != TypeKind::Interface)
    return false;

  auto &iface_info = std::get<InterfaceTypeInfo>(iface->detail);

  // Collect methods from the concrete type.
  std::vector<MethodInfo> concrete_methods;

  if (concrete->kind == TypeKind::Struct) {
    auto &s = std::get<StructTypeInfo>(concrete->detail);
    concrete_methods = s.methods;
  } else {
    // Check stdlib-defined receiver methods (type_methods_ for scalar types).
    const Type *raw = concrete.get();
    auto tm_it = type_methods_.find(raw);
    if (tm_it == type_methods_.end()) {
      const Type *canonical = nullptr;
      switch (concrete->kind) {
      case TypeKind::Int:    canonical = builtins.int_type.get(); break;
      case TypeKind::Float:  canonical = builtins.float_type.get(); break;
      case TypeKind::Bool:   canonical = builtins.bool_type.get(); break;
      case TypeKind::String: canonical = builtins.string_type.get(); break;
      default: break;
      }
      if (canonical && canonical != raw)
        tm_it = type_methods_.find(canonical);
    }
    if (tm_it != type_methods_.end()) {
      for (auto &m : tm_it->second)
        concrete_methods.push_back(m);
    }
    // Check stdlib-defined generic receiver methods (kind_methods_ for
    // Array, Map).
    auto km_it = kind_methods_.find(concrete->kind);
    if (km_it != kind_methods_.end()) {
      for (auto &m : km_it->second)
        concrete_methods.push_back(m);
    }
    // Check remaining built-in methods (Enum, Range, Array/Map String).
    auto bm = builtin_methods(concrete->kind, builtins);
    for (auto &m : bm)
      concrete_methods.push_back(m);
  }

  // Every interface method must be present on the concrete type with a
  // compatible signature.
  for (auto &im : iface_info.methods) {
    bool found = false;
    for (auto &cm : concrete_methods) {
      if (cm.name == im.name) {
        found = true;
        if (im.signature && cm.signature) {
          if (!types_equal(im.signature, cm.signature)) {
            return false; // Signature mismatch.
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

} // namespace saga

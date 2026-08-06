// Copywrite 2026 Rob Thornton
// SPDX-License-Identifier: MIT

// The first pass: registering every top-level name before anything is typed,
// so a declaration may refer to one that appears later in the file. Nothing
// here resolves a type — only that a name exists and what kind of thing it is.

#include "semantic/analyzer.hpp"
#include "semantic/analyzer_detail.hpp"
#include <format>

namespace saga {

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

} // namespace saga

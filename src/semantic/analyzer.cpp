// Copywrite 2026 Rob Thornton
// SPDX-License-Identifier: MIT

#include "semantic/analyzer.hpp"

#include <format>

namespace mc {

// ===========================================================================
// Construction
// ===========================================================================

Analyzer::Analyzer(FileSet &fs)
    : fileset(fs),
      global_scope(std::make_shared<Scope>(ScopeKind::Global)),
      current_scope(global_scope) {
  register_builtins(global_scope, builtins);
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

    // Register the type param in the current scope.
    declare(Symbol::type_param(tp.name, tp_type, ident.span));

    // Also store in the scope's type_bindings for later substitution.
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
  node_types[&node] = std::move(type);
}

void Analyzer::record_symbol(const Node &node, const Symbol &sym) {
  node_symbols[&node] = sym;
}

// ===========================================================================
// Error reporting
// ===========================================================================

void Analyzer::error(Span span, const std::string &message) {
  // Use the first file's position_at for now; a proper multi-file lookup
  // will be added when module support lands.
  Position pos{};
  if (!fileset.files.empty()) {
    pos = fileset.files[0]->position_at(span.start);
  }
  errors.report_error(pos, message);
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
// Phase 1 stubs — declaration collection
// ===========================================================================

void Analyzer::visit_package(const PackageNode &pkg) {
  for (auto &src : pkg.sources) {
    auto &src_node = std::get<SourceNode>(src->data);
    visit_source(src_node);
  }
}

void Analyzer::visit_source(const SourceNode &src) {
  push_scope(ScopeKind::Module);

  // Pass 1: collect all top-level names (forward declarations).
  for (auto &decl : src.declarations) {
    collect_declaration(*decl);
  }

  // Pass 2: type-check bodies (to be implemented in later phases).
  // for (auto &decl : src.declarations) {
  //   check_declaration(*decl);
  // }

  pop_scope();
}

void Analyzer::collect_declaration(const Node &node) {
  std::visit(
      overloaded{
          [&](const FuncDeclNode &fn) {
            declare(Symbol::function(std::string(fn.name.name), nullptr,
                                     fn.name.span, fn.is_public));
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
          [&](const ImportDeclNode &) {
            // Import handling deferred to module support.
          },
          [&](const auto &) {
            error(node.span, "unexpected node at top level");
          },
      },
      node.data);
}

// ===========================================================================
// Phase 2 stubs — type resolution
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
          [&](const MapTypeNode &n) -> TypePtr {
            return resolve_map_type(n);
          },
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
    error(node.span,
          std::format("'{}' is not a type", std::string(node.name)));
    return builtins.error_type;
  }
  return sym->type ? sym->type : builtins.error_type;
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

// ===========================================================================
// Phase 3–5 stubs — to be implemented in later steps
// ===========================================================================

TypePtr Analyzer::check_expr(const Node & /*node*/) {
  return builtins.error_type; // TODO
}

TypePtr Analyzer::check_identifier(const IdentifierNode & /*node*/,
                                   const Node & /*parent*/) {
  return builtins.error_type; // TODO
}

TypePtr Analyzer::check_bool_literal(const BoolLiteralNode & /*node*/) {
  return builtins.bool_type;
}

TypePtr Analyzer::check_int_literal(const IntegerLiteralNode & /*node*/) {
  return builtins.int_type;
}

TypePtr Analyzer::check_float_literal(const FloatLiteralNode & /*node*/) {
  return builtins.float_type;
}

TypePtr Analyzer::check_string_literal(const StringLiteralNode & /*node*/) {
  return builtins.string_type;
}

TypePtr Analyzer::check_array_literal(const ArrayLiteralNode & /*node*/) {
  return builtins.error_type; // TODO
}

TypePtr Analyzer::check_map_literal(const MapLiteralNode & /*node*/) {
  return builtins.error_type; // TODO
}

TypePtr Analyzer::check_struct_literal(const StructLiteralNode & /*node*/) {
  return builtins.error_type; // TODO
}

TypePtr Analyzer::check_binary_expr(const BinaryExprNode & /*node*/) {
  return builtins.error_type; // TODO
}

TypePtr Analyzer::check_unary_expr(const UnaryExprNode & /*node*/) {
  return builtins.error_type; // TODO
}

TypePtr Analyzer::check_call_expr(const CallExprNode & /*node*/) {
  return builtins.error_type; // TODO
}

TypePtr Analyzer::check_index_expr(const IndexExprNode & /*node*/) {
  return builtins.error_type; // TODO
}

TypePtr Analyzer::check_selector(const SelectorNode & /*node*/,
                                 const Node & /*parent*/) {
  return builtins.error_type; // TODO
}

TypePtr Analyzer::check_if_expr(const IfExprNode & /*node*/) {
  return builtins.error_type; // TODO
}

TypePtr Analyzer::check_switch_expr(const SwitchExprNode & /*node*/) {
  return builtins.error_type; // TODO
}

TypePtr Analyzer::check_for_expr(const ForExprNode & /*node*/) {
  return builtins.error_type; // TODO
}

TypePtr Analyzer::check_range_expr(const RangeExprNode & /*node*/) {
  return builtins.error_type; // TODO
}

TypePtr Analyzer::check_spawn_expr(const SpawnExprNode & /*node*/) {
  return builtins.error_type; // TODO
}

TypePtr Analyzer::check_or_expr(const OrExprNode & /*node*/) {
  return builtins.error_type; // TODO
}

TypePtr Analyzer::check_func_expr(const FuncExprNode & /*node*/) {
  return builtins.error_type; // TODO
}

TypePtr Analyzer::check_group_expr(const GroupExprNode & /*node*/) {
  return builtins.error_type; // TODO
}

TypePtr Analyzer::check_import_expr(const ImportExprNode & /*node*/) {
  return builtins.error_type; // TODO
}

void Analyzer::check_stmt(const Node & /*node*/) {
  // TODO
}

void Analyzer::check_var_decl(const VarDeclNode & /*node*/,
                              const Node & /*parent*/) {
  // TODO
}

void Analyzer::check_decl_assign(const DeclAssignNode & /*node*/) {
  // TODO
}

void Analyzer::check_assign(const AssignNode & /*node*/) {
  // TODO
}

void Analyzer::check_increment(const IncrementNode & /*node*/) {
  // TODO
}

void Analyzer::check_decrement(const DecrementNode & /*node*/) {
  // TODO
}

void Analyzer::check_return(const ReturnNode & /*node*/) {
  // TODO
}

void Analyzer::check_break(const BreakNode & /*node*/) {
  // TODO
}

void Analyzer::check_next(const NextNode & /*node*/) {
  // TODO
}

void Analyzer::check_const_decl(const ConstDeclNode & /*node*/) {
  // TODO
}

void Analyzer::check_enum_decl(const EnumDeclNode & /*node*/) {
  // TODO
}

void Analyzer::check_func_decl(const FuncDeclNode & /*node*/) {
  // TODO
}

void Analyzer::check_struct_decl(const StructDeclNode & /*node*/) {
  // TODO
}

void Analyzer::check_interface_decl(const InterfaceDeclNode & /*node*/) {
  // TODO
}

void Analyzer::check_import_decl(const ImportDeclNode & /*node*/) {
  // TODO
}

TypePtr Analyzer::check_block(const BlockNode & /*node*/) {
  return builtins.void_type; // TODO
}

TypePtr Analyzer::instantiate_generic_call(
    const TypePtr & /*callee_type*/,
    const std::vector<TypeParam> & /*type_params*/,
    const std::vector<TypePtr> & /*arg_types*/, Span /*call_span*/) {
  return builtins.error_type; // TODO
}

bool Analyzer::satisfies_interface(const TypePtr & /*concrete*/,
                                   const TypePtr & /*iface*/) {
  return false; // TODO
}

} // namespace mc

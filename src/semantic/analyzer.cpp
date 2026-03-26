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
  node_types[&node] = std::move(type);
}

void Analyzer::record_symbol(const Node &node, const Symbol &sym) {
  node_symbols[&node] = sym;
}

// ===========================================================================
// Error reporting
// ===========================================================================

void Analyzer::error(Span span, const std::string &message) {
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

  // Pass 2: resolve declaration types (struct fields, signatures, etc.).
  for (auto &decl : src.declarations) {
    resolve_declaration(*decl);
  }

  // Pass 3: resolve names inside function/method bodies.
  for (auto &decl : src.declarations) {
    std::visit(
        overloaded{
            [&](const FuncDeclNode &fn) {
              resolve_func_decl_body(fn);
            },
            [&](const StructDeclNode &s) {
              // Resolve names inside struct method bodies.
              for (auto &member : s.members) {
                if (auto *fn =
                        std::get_if<FuncDeclNode>(&member.member->data)) {
                  resolve_func_decl_body(*fn);
                }
              }
            },
            [&](const auto &) {
              // Other declarations have no bodies to walk.
            },
        },
        decl->data);
  }

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
  std::visit(
      overloaded{
          [&](const FuncDeclNode &fn) { resolve_func_decl(fn); },
          [&](const StructDeclNode &s) { resolve_struct_decl(s); },
          [&](const EnumDeclNode &e) { resolve_enum_decl(e); },
          [&](const InterfaceDeclNode &i) { resolve_interface_decl(i); },
          [&](const ConstDeclNode &c) { resolve_const_decl(c); },
          [&](const ImportDeclNode &) { /* deferred */ },
          [&](const auto &) { /* already reported in collect */ },
      },
      node.data);
}

void Analyzer::resolve_func_decl(const FuncDeclNode &fn) {
  // Resolve the function's type signature and update the symbol.
  auto fn_type = resolve_signature(fn.signature);
  auto sym_it = current_scope->symbols.find(std::string(fn.name.name));
  if (sym_it != current_scope->symbols.end()) {
    sym_it->second.type = fn_type;
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
    std::visit(
        overloaded{
            [&](const FieldSpecNode &fs) {
              auto ft = resolve_type(*fs.type);
              for (auto &ident : fs.names.identifiers) {
                fields.push_back(
                    {std::string(ident.name), ft, member.is_public});
              }
            },
            [&](const FuncDeclNode &fn) {
              auto fn_type = resolve_signature(fn.signature);
              methods.push_back(
                  {std::string(fn.name.name), fn_type, member.is_public});
            },
            [&](const auto &) {},
        },
        member.member->data);
  }

  // Resolve embeds.
  std::vector<TypePtr> embeds;
  for (auto &embed_ident : s.embeds) {
    auto sym = lookup(std::string(embed_ident.name));
    if (!sym) {
      undefined_error(embed_ident.span, std::string(embed_ident.name));
    } else if (sym->kind != SymbolKind::Type) {
      error(embed_ident.span,
            std::format("'{}' is not a type", std::string(embed_ident.name)));
    } else if (sym->type) {
      embeds.push_back(sym->type);
    }
  }

  auto struct_type = make_struct_type(std::string(s.name.name),
                                      std::move(fields), std::move(methods),
                                      std::move(type_params));
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
  for (auto &field : e.fields) {
    std::vector<FieldInfo> variant_fields;
    for (auto &fa : field.initializer) {
      // Enum field initializers are constant expressions; for now we just
      // record the name. Type-checking of values is deferred.
      variant_fields.push_back(
          {std::string(fa.name.name), nullptr, false});
    }
    variants.push_back({std::string(field.name.name),
                        std::move(variant_fields)});
  }

  auto enum_type =
      make_enum_type(std::string(e.name.name), std::move(variants));

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
    methods.push_back(
        {std::string(field.name.name), fn_type, field.is_public});
  }

  auto iface_type = make_interface_type(
      std::string(i.name.name), std::move(methods), std::move(type_params));

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
void Analyzer::resolve_func_decl_body(const FuncDeclNode &fn) {
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
          [&](const IdentifierNode &n) {
            resolve_identifier(n, node);
          },
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
          [&](const SpawnExprNode &n) { resolve_spawn_expr(n); },
          [&](const OrExprNode &n) { resolve_or_expr(n); },
          [&](const FuncExprNode &n) { resolve_func_expr(n); },
          [&](const ImportExprNode &) { /* deferred to module support */ },
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
}

void Analyzer::resolve_block(const BlockNode &block) {
  for (auto &stmt : block.stmts) {
    resolve_block_stmt(*stmt);
  }
}

void Analyzer::resolve_block_stmt(const Node &node) {
  // Dispatch: some nodes are statements that introduce names,
  // others are expressions.
  std::visit(
      overloaded{
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
    std::visit(
        overloaded{
            [&](const ForRangeClauseNode &range) {
              // Resolve the iterable expression first.
              resolve_expr(*range.iterable);
              // Declare the loop variable(s) into the loop scope.
              for (auto &var : range.vars) {
                declare_local(Symbol::variable(
                    std::string(var.name), nullptr, var.span));
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
    declare_local(Symbol::variable(std::string(node.accumulator->name),
                                   nullptr, node.accumulator->span));
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

void Analyzer::resolve_spawn_expr(const SpawnExprNode &node) {
  push_scope(ScopeKind::Spawn);

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

void Analyzer::resolve_func_expr(const FuncExprNode &node) {
  push_scope(ScopeKind::Function);

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

  pop_scope();
}

// ===========================================================================
// Phase 4 — Statement name resolution
// ===========================================================================

void Analyzer::resolve_stmt(const Node &node) {
  resolve_block_stmt(node);
}

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
// Type-checking stubs — to be implemented in later steps
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

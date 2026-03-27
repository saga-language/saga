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
              for (auto &member : s.members) {
                if (auto *fn =
                        std::get_if<FuncDeclNode>(&member.member->data)) {
                  resolve_func_decl_body(*fn);
                }
              }
            },
            [&](const auto &) {},
        },
        decl->data);
  }

  // Pass 4: type-check top-level declarations and function bodies.
  for (auto &decl : src.declarations) {
    std::visit(
        overloaded{
            [&](const FuncDeclNode &fn) { check_func_decl_body(fn); },
            [&](const StructDeclNode &s) {
              check_struct_decl(s);
              for (auto &member : s.members) {
                if (auto *fn =
                        std::get_if<FuncDeclNode>(&member.member->data)) {
                  check_func_decl_body(*fn);
                }
              }
            },
            [&](const EnumDeclNode &e) { check_enum_decl(e); },
            [&](const InterfaceDeclNode &i) { check_interface_decl(i); },
            [&](const ConstDeclNode &c) { check_const_decl(c); },
            [&](const ImportDeclNode &) { /* deferred */ },
            [&](const auto &) {},
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
  // If the function is generic, push a temporary scope to hold type params
  // so the signature can reference them.
  bool has_generics = fn.generic.has_value();
  if (has_generics) {
    push_scope(ScopeKind::Block);
    enter_generics(*fn.generic);
  }

  auto fn_type = resolve_signature(fn.signature);

  // Mark the function type as variadic if the last param is.
  if (!fn.signature.params.empty() && fn.signature.params.back().is_variadic) {
    auto &fi = std::get<FuncTypeInfo>(fn_type->detail);
    fi.is_variadic = true;
  }

  if (has_generics)
    pop_scope();

  // If this is a receiver method, attach it to the receiver's struct type.
  if (fn.receiver) {
    auto &recv_type_node = fn.receiver->type;
    if (auto *ident = std::get_if<IdentifierNode>(&recv_type_node->data)) {
      auto recv_sym = lookup(std::string(ident->name));
      if (recv_sym && recv_sym->type &&
          recv_sym->type->kind == TypeKind::Struct) {
        auto &struct_info =
            std::get<StructTypeInfo>(recv_sym->type->detail);
        struct_info.methods.push_back(
            {std::string(fn.name.name), fn_type, fn.is_public});
      }
    }
  }

  // Update the function symbol.
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
// Phase 4 — Type-check function/method bodies
// ===========================================================================

void Analyzer::check_func_decl_body(const FuncDeclNode &fn) {
  push_scope(ScopeKind::Function);

  if (fn.generic)
    enter_generics(*fn.generic);

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
  // If the last statement is a return, it already checked its own values.
  if (!current_scope->return_types.empty() && !block.stmts.empty()) {
    bool tail_is_return =
        std::holds_alternative<ReturnNode>(block.stmts.back()->data);
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
            return check_binary_expr(n);
          },
          [&](const UnaryExprNode &n) -> TypePtr {
            return check_unary_expr(n);
          },
          [&](const GroupExprNode &n) -> TypePtr {
            return check_group_expr(n);
          },
          [&](const CallExprNode &n) -> TypePtr {
            return check_call_expr(n);
          },
          [&](const IndexExprNode &n) -> TypePtr {
            return check_index_expr(n);
          },
          [&](const SelectorNode &n) -> TypePtr {
            return check_selector(n, node);
          },
          [&](const IfExprNode &n) -> TypePtr {
            return check_if_expr(n);
          },
          [&](const SwitchExprNode &n) -> TypePtr {
            return check_switch_expr(n);
          },
          [&](const ForExprNode &n) -> TypePtr {
            return check_for_expr(n);
          },
          [&](const RangeExprNode &n) -> TypePtr {
            return check_range_expr(n);
          },
          [&](const SpawnExprNode &n) -> TypePtr {
            return check_spawn_expr(n);
          },
          [&](const OrExprNode &n) -> TypePtr {
            return check_or_expr(n);
          },
          [&](const FuncExprNode &n) -> TypePtr {
            return check_func_expr(n);
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
          [&](const NextNode &) -> TypePtr {
            return builtins.void_type;
          },
          [&](const auto &) -> TypePtr {
            return builtins.error_type;
          },
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
      expect_assignable(node.elements[i]->span, elem_type, t,
                        "array element");
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
      expect_assignable(node.entries[i].value->span, val_type, vt,
                        "map value");
  }
  return make_map_type(key_type, val_type);
}

TypePtr Analyzer::check_struct_literal(const StructLiteralNode &node) {
  auto type_expr_type = check_expr(*node.type_expr);
  if (is_error_type(type_expr_type))
    return builtins.error_type;

  if (type_expr_type->kind != TypeKind::Struct) {
    error(node.type_expr->span,
          std::format("'{}' is not a struct type",
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
  auto &raw_info = std::get<StructTypeInfo>(type_expr_type->detail);
  auto effective_type = type_expr_type;
  if (!raw_info.type_params.empty()) {
    effective_type =
        instantiate_generic_struct(type_expr_type, field_vals, node.span);
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
  for (auto &[fname, val_type] : field_vals) {
    bool found = false;
    for (auto &fi : all_fields) {
      if (fi.name == fname) {
        found = true;
        if (fi.type && !is_error_type(val_type)) {
          expect_assignable(node.span, fi.type, val_type,
                            std::format("field '{}'", fname));
        }
        break;
      }
    }
    if (!found) {
      error(node.span,
            std::format("struct '{}' has no field '{}'", info.name, fname));
    }
  }

  return effective_type;
}

TypePtr Analyzer::check_binary_expr(const BinaryExprNode &node) {
  auto lhs = check_expr(*node.lhs);
  auto rhs = check_expr(*node.rhs);

  if (is_error_type(lhs) || is_error_type(rhs))
    return builtins.error_type;

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
    if (!is_equatable(lhs)) {
      error(node.lhs->span,
            std::format("type {} does not support equality",
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
      error(node.lhs->span,
            std::format("type {} does not support ordering",
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

TypePtr Analyzer::check_call_expr(const CallExprNode &node) {
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
    auto instantiated =
        instantiate_generic_call(callee_type, arg_types, node.span);
    if (!is_error_type(instantiated))
      effective_type = instantiated;
  }

  auto &fn_info = std::get<FuncTypeInfo>(effective_type->detail);

  // Check argument count.
  if (!fn_info.is_variadic) {
    if (arg_types.size() != fn_info.params.size()) {
      error(node.span,
            std::format("expected {} argument(s), got {}",
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
    if (i < fn_info.params.size()) {
      expect_assignable(node.args[i]->span, fn_info.params[i], arg_types[i],
                        std::format("argument {}", i + 1));
    } else if (fn_info.is_variadic && !fn_info.params.empty()) {
      auto &last = fn_info.params.back();
      if (last->kind == TypeKind::Array) {
        auto &arr = std::get<ArrayTypeInfo>(last->detail);
        expect_assignable(node.args[i]->span, arr.element, arg_types[i],
                          std::format("variadic argument {}", i + 1));
      }
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
      expect_assignable(node.index->span, map_info.key, index_type,
                        "map key");
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
    error(node.span,
          std::format("type {} does not support indexing",
                      type_to_string(obj_type)));
    return builtins.error_type;
  }
}

TypePtr Analyzer::check_selector(const SelectorNode &node,
                                 const Node &parent) {
  auto obj_type = check_expr(*node.object);
  if (is_error_type(obj_type))
    return builtins.error_type;

  std::string field_name(node.field.name);

  // Look up fields and methods on the type.
  if (obj_type->kind == TypeKind::Struct) {
    auto &info = std::get<StructTypeInfo>(obj_type->detail);
    for (auto &f : info.fields) {
      if (f.name == field_name)
        return f.type ? f.type : builtins.error_type;
    }
    for (auto &m : info.methods) {
      if (m.name == field_name)
        return m.signature ? m.signature : builtins.error_type;
    }
    // Check embedded types.
    for (auto &embed : info.embeds) {
      if (embed && embed->kind == TypeKind::Struct) {
        auto &embed_info = std::get<StructTypeInfo>(embed->detail);
        for (auto &f : embed_info.fields) {
          if (f.name == field_name)
            return f.type ? f.type : builtins.error_type;
        }
        for (auto &m : embed_info.methods) {
          if (m.name == field_name)
            return m.signature ? m.signature : builtins.error_type;
        }
      }
    }
  }

  if (obj_type->kind == TypeKind::Enum) {
    auto &info = std::get<EnumTypeInfo>(obj_type->detail);
    for (auto &v : info.variants) {
      if (v.name == field_name)
        return obj_type; // Enum.Variant has the enum type itself.
    }
  }

  // Check built-in methods for the type kind.
  auto methods = builtin_methods(obj_type->kind, builtins);
  for (auto &m : methods) {
    if (m.name == field_name)
      return m.signature ? m.signature : builtins.error_type;
  }

  error(node.field.span,
        std::format("type {} has no member '{}'", type_to_string(obj_type),
                    field_name));
  return builtins.error_type;
}

TypePtr Analyzer::check_if_expr(const IfExprNode &node) {
  auto cond_type = check_expr(*node.condition);
  expect_bool(node.condition->span, cond_type);

  push_scope(ScopeKind::Block);
  auto &then_block = std::get<BlockNode>(node.then_block->data);
  auto then_type = check_block(then_block);
  pop_scope();

  if (node.else_block) {
    push_scope(ScopeKind::Block);
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

  for (auto &arm : node.arms) {
    auto pattern_type = check_expr(*arm.pattern);
    // Value matching: pattern must be same type as subject.
    if (!is_error_type(pattern_type) && !is_error_type(subject_type)) {
      expect_assignable(arm.pattern->span, subject_type, pattern_type,
                        "case pattern");
    }

    TypePtr arm_type;
    if (auto *block = std::get_if<BlockNode>(&arm.body->data)) {
      push_scope(ScopeKind::Block);
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

TypePtr Analyzer::check_for_expr(const ForExprNode &node) {
  push_scope(ScopeKind::Loop);

  if (node.mode) {
    std::visit(
        overloaded{
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

  // Accumulator pipe.
  if (node.accumulator) {
    current_scope->symbols.emplace(
        std::string(node.accumulator->name),
        Symbol::variable(std::string(node.accumulator->name), nullptr,
                         node.accumulator->span));
  }

  auto &body_block = std::get<BlockNode>(node.body->data);
  check_block(body_block);

  pop_scope();
  return builtins.void_type;
}

TypePtr Analyzer::check_range_expr(const RangeExprNode &node) {
  auto low = check_expr(*node.low);
  auto high = check_expr(*node.high);

  if (!is_error_type(low) && !is_numeric(low)) {
    error(node.low->span,
          std::format("range requires numeric type, got {}",
                      type_to_string(low)));
  }
  if (!is_error_type(high) && !is_numeric(high)) {
    error(node.high->span,
          std::format("range requires numeric type, got {}",
                      type_to_string(high)));
  }

  auto elem = common_type(low, high);
  return make_range_type(elem ? elem : builtins.int_type);
}

TypePtr Analyzer::check_spawn_expr(const SpawnExprNode &node) {
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

  pop_scope();
  return builtins.task_type;
}

TypePtr Analyzer::check_or_expr(const OrExprNode &node) {
  auto expr_type = check_expr(*node.expr);

  // The expression should be an impure type (contains Error).
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

  if (is_error_type(expr_type))
    return builtins.error_type;

  // Strip Error/Missing from the union to get the purified type.
  if (expr_type->kind == TypeKind::Union) {
    auto &info = std::get<UnionTypeInfo>(expr_type->detail);
    std::vector<TypePtr> purified;
    for (auto &alt : info.alternatives) {
      if (alt->kind != TypeKind::Interface ||
          std::get<InterfaceTypeInfo>(alt->detail).name != "Error") {
        purified.push_back(alt);
      }
    }
    if (purified.empty())
      return fallback_type;
    if (purified.size() == 1)
      return purified[0];
    return make_union_type(std::move(purified));
  }

  return expr_type;
}

TypePtr Analyzer::check_func_expr(const FuncExprNode &node) {
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
          Symbol::parameter(std::string(ident.name), param_type,
                            ident.span));
    }
  }

  auto &block = std::get<BlockNode>(node.body->data);
  auto body_type = check_block(block);

  // Check tail expression matches return type (unless tail is a return).
  auto &fn_info = std::get<FuncTypeInfo>(fn_type->detail);
  bool tail_is_return =
      !block.stmts.empty() &&
      std::holds_alternative<ReturnNode>(block.stmts.back()->data);
  if (!tail_is_return && fn_info.returns.size() == 1 &&
      !is_error_type(body_type)) {
    if (!types_equal(fn_info.returns[0], builtins.void_type)) {
      expect_assignable(node.body->span, fn_info.returns[0], body_type,
                        "return type");
    }
  }

  pop_scope();
  return fn_type;
}

TypePtr Analyzer::check_import_expr(const ImportExprNode &) {
  // Module support deferred.
  return builtins.error_type;
}

// ===========================================================================
// Statement type-checking
// ===========================================================================

void Analyzer::check_stmt(const Node &node) {
  std::visit(
      overloaded{
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
  if (var.type)
    declared_type = resolve_type(**var.type);

  TypePtr final_type = declared_type;

  if (var.init) {
    auto init_type = check_expr(**var.init);
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
        expect_assignable(node.values[i]->span, builtins.string_type,
                          val_type, "string concatenation assignment");
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
    error(node.span,
          std::format("increment requires integer type, got {}",
                      type_to_string(t)));
  }
}

void Analyzer::check_decrement(const DecrementNode &node) {
  auto t = check_expr(*node.operand);
  if (!is_error_type(t) && t->kind != TypeKind::Int) {
    error(node.span,
          std::format("decrement requires integer type, got {}",
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
    error(node.span,
          std::format("return has {} value(s), expected {}",
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

// ===========================================================================
// Top-level declaration type-checking
// ===========================================================================

void Analyzer::check_const_decl(const ConstDeclNode &c) {
  TypePtr declared_type = nullptr;
  if (c.type)
    declared_type = resolve_type(**c.type);

  auto init_type = check_expr(*c.value);

  if (declared_type && !is_error_type(init_type)) {
    expect_assignable(c.value->span, declared_type, init_type,
                      "constant initializer");
  }

  // Update symbol type.
  auto sym_it = current_scope->symbols.find(std::string(c.name.name));
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
      error(s.span,
            std::format("duplicate field '{}' in struct '{}'", f.name,
                        info.name));
    }
    seen_fields[f.name] = true;
  }

  // Check duplicate methods.
  std::unordered_map<std::string, bool> seen_methods;
  for (auto &m : info.methods) {
    if (seen_methods.count(m.name)) {
      error(s.span,
            std::format("duplicate method '{}' in struct '{}'", m.name,
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
      error(i.span,
            std::format("duplicate method '{}' in interface '{}'", m.name,
                        info.name));
    }
    seen[m.name] = true;
  }
}

void Analyzer::check_import_decl(const ImportDeclNode &) {
  // Deferred to module support.
}

// ===========================================================================
// Generic instantiation
// ===========================================================================

TypePtr Analyzer::instantiate_generic_call(
    const TypePtr &callee_type, const std::vector<TypePtr> &arg_types,
    Span call_span) {
  if (!callee_type || callee_type->kind != TypeKind::Func)
    return builtins.error_type;

  auto &fn_info = std::get<FuncTypeInfo>(callee_type->detail);
  std::unordered_map<uint32_t, TypePtr> bindings;

  // Attempt to unify each parameter type with the argument type.
  size_t count = std::min(fn_info.params.size(), arg_types.size());
  for (size_t i = 0; i < count; ++i) {
    if (!unify(fn_info.params[i], arg_types[i], bindings)) {
      error(call_span,
            std::format("cannot infer type parameter from argument {}",
                        i + 1));
      return builtins.error_type;
    }
  }

  if (bindings.empty())
    return callee_type; // No type params to substitute.

  return substitute(callee_type, bindings);
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
          error(span,
                std::format("cannot infer type parameter from field '{}'",
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
        {m.name, substitute(m.signature, bindings), m.is_public});
  }

  auto result = make_struct_type(info.name, std::move(new_fields),
                                 std::move(new_methods));
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
    // Check built-in methods.
    concrete_methods = builtin_methods(concrete->kind, builtins);
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

} // namespace mc

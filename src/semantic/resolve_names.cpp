// Copywrite 2026 Rob Thornton
// SPDX-License-Identifier: MIT

// Binding every identifier in a function body to what it names, before any
// type is asked for. Resolution and checking are separate passes because a
// name may be declared after the expression that uses it.

#include "semantic/analyzer.hpp"
#include <format>

namespace saga {

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

// Copyright 2026 Rob Thornton
// SPDX-License-Identifier: MIT

// Whether an expression's value is owned or borrowed, read off its shape.
// Anything not known to make a fresh value is borrowed, so a producer missing
// from this list costs a leak rather than a double free. A conditional is owned
// when any of its branches is: the join gives each borrowed branch a reference
// of its own, so the consumer sees one answer for the whole expression.

#include "ir/codegen.hpp"

namespace saga {

namespace {
Ownership either(Ownership a, Ownership b) {
  return a == Ownership::Owned ? a : b;
}

bool is_slice(const IndexExprNode &node) {
  return std::holds_alternative<SliceNode>(node.index->data);
}

// A statement in tail position leaves its block without a value of its own.
bool is_statement(const Node &node) {
  return std::holds_alternative<VarDeclNode>(node.data) ||
         std::holds_alternative<DeclAssignNode>(node.data) ||
         std::holds_alternative<DestructureNode>(node.data) ||
         std::holds_alternative<AssignNode>(node.data) ||
         std::holds_alternative<IncrementNode>(node.data) ||
         std::holds_alternative<DecrementNode>(node.data) ||
         std::holds_alternative<ReturnNode>(node.data) ||
         std::holds_alternative<BreakNode>(node.data) ||
         std::holds_alternative<NextNode>(node.data);
}

const Node *block_tail(const BlockNode &block) {
  if (block.stmts.empty() || is_statement(*block.stmts.back()))
    return nullptr;
  return block.stmts.back().get();
}

bool within(Span inner, Span outer) {
  return outer.start <= inner.start && inner.end <= outer.end;
}
} // namespace

Ownership CodeGen::value_ownership(const Node &node) {
  using enum Ownership;
  return std::visit(
      overloaded{
          [&](const GroupExprNode &n) { return value_ownership(*n.inner); },
          [&](const PromoteExprNode &n) { return value_ownership(*n.operand); },
          [&](const StringLiteralNode &n) {
            return interpolates(n) ? Owned : Borrowed;
          },
          [&](const ArrayLiteralNode &) { return Owned; },
          [&](const MapLiteralNode &) { return Owned; },
          [&](const StructLiteralNode &) { return Owned; },
          [&](const RangeNode &) { return Owned; },
          [&](const BinaryExprNode &) { return Owned; },
          [&](const SpawnExprNode &) { return Owned; },
          [&](const FuncExprNode &) { return Owned; },
          [&](const ForExprNode &n) {
            return n.accumulator || break_value_type(n, semantic_type(node))
                       ? Owned
                       : Borrowed;
          },
          [&](const CallExprNode &n) { return call_ownership(n); },
          [&](const IndexExprNode &n) {
            return is_slice(n) || indexes_string(n) ? Owned : Borrowed;
          },
          [&](const OrExprNode &n) { return or_ownership(n); },
          [&](const IfExprNode &n) { return if_ownership(n, node); },
          [&](const SwitchExprNode &n) { return switch_ownership(n, node); },
          [&](const auto &) { return Borrowed; },
      },
      node.data);
}

// A string's element is a string of its own, made for the read.
bool CodeGen::indexes_string(const IndexExprNode &node) {
  auto sem = semantic_type(*node.object);
  return sem && sem->kind == TypeKind::String;
}

Ownership CodeGen::call_ownership(const CallExprNode &call) {
  return reads_stored_element(call) ? Ownership::Borrowed : Ownership::Owned;
}

// A collection method returning its T reads that T out of the collection, and
// is written once over an opaque T, so it cannot take a reference for it. One
// that writes through its receiver (`Pop`) takes the T out instead, and hands
// over the collection's reference with it.
bool CodeGen::reads_stored_element(const CallExprNode &call) {
  auto *sel = std::get_if<SelectorNode>(&call.callee->data);
  if (!sel)
    return false;
  auto obj_sem = unwrap_alias(semantic_type(*sel->object));
  if (!obj_sem || (obj_sem->kind != TypeKind::Array &&
                   obj_sem->kind != TypeKind::Map))
    return false;
  auto km_it = analyzer.kind_methods_.find(obj_sem->kind);
  if (km_it == analyzer.kind_methods_.end())
    return false;
  std::string method(sel->field.name);
  auto *fi = method_signature(km_it->second, method);
  return fi && fi->return_type &&
         fi->return_type->kind == TypeKind::TypeParam &&
         !kind_method_mutates(obj_sem, method);
}

// A branch with no value of its own yields the result type's zero, and an
// empty collection is a fresh one.
Ownership CodeGen::zero_ownership(const TypePtr &result) {
  auto sem = unwrap_alias(result);
  return sem && (sem->kind == TypeKind::Array || sem->kind == TypeKind::Map)
             ? Ownership::Owned
             : Ownership::Borrowed;
}

// A concrete branch of an interface-typed conditional reaches the join in a
// box made for it.
Ownership CodeGen::body_ownership(const Node *body, const TypePtr &result) {
  if (body && boxes_into(body_result_type(*body), result))
    return Ownership::Owned;
  return branch_value_ownership(body, result);
}

// A value read out of one of the branch's own locals would go with the block,
// so the join retains it first and the branch hands it over owned.
Ownership CodeGen::branch_value_ownership(const Node *body,
                                          const TypePtr &result) {
  if (lends_from_block(body))
    return Ownership::Owned;
  return branch_tail_ownership(body, result);
}

Ownership CodeGen::branch_tail_ownership(const Node *body,
                                         const TypePtr &result) {
  if (!body)
    return zero_ownership(result);
  auto *block = std::get_if<BlockNode>(&body->data);
  if (!block)
    return value_ownership(*body);
  auto *tail = block_tail(*block);
  return tail ? value_ownership(*tail) : zero_ownership(result);
}

bool CodeGen::lends_from_block(const Node *body) {
  auto *block = body ? std::get_if<BlockNode>(&body->data) : nullptr;
  return block && body_lends_from(body, block->span);
}

bool CodeGen::body_lends_from(const Node *body, Span scope) {
  if (!body)
    return false;
  auto *block = std::get_if<BlockNode>(&body->data);
  auto *tail = block ? block_tail(*block) : body;
  return tail && value_ownership(*tail) == Ownership::Borrowed &&
         lends_from(*tail, scope);
}

// Whether a borrowed value may be read out of a local declared within `scope`;
// a conditional may hand over the value of any of its branches.
bool CodeGen::lends_from(const Node &node, Span scope) {
  return std::visit(
      overloaded{
          [&](const IdentifierNode &) {
            auto *sym = node_symbol(node);
            return sym && within(sym->decl_span, scope);
          },
          [&](const SelectorNode &n) { return lends_from(*n.object, scope); },
          [&](const IndexExprNode &n) { return lends_from(*n.object, scope); },
          [&](const GroupExprNode &n) { return lends_from(*n.inner, scope); },
          [&](const PromoteExprNode &n) {
            return lends_from(*n.operand, scope);
          },
          [&](const CallExprNode &n) {
            return reads_stored_element(n) &&
                   lends_from(*std::get<SelectorNode>(n.callee->data).object,
                              scope);
          },
          [&](const OrExprNode &n) {
            return lends_from(*n.expr, scope) ||
                   body_lends_from(n.fallback.get(), scope);
          },
          [&](const IfExprNode &n) {
            return body_lends_from(n.then_block.get(), scope) ||
                   (n.else_block &&
                    body_lends_from(n.else_block->get(), scope));
          },
          [&](const SwitchExprNode &n) { return switch_lends_from(n, scope); },
          [&](const auto &) { return false; },
      },
      node.data);
}

bool CodeGen::switch_lends_from(const SwitchExprNode &node, Span scope) {
  for (auto &arm : node.arms)
    if (body_lends_from(arm.body.get(), scope))
      return true;
  return node.else_body && body_lends_from(node.else_body->get(), scope);
}

Ownership CodeGen::or_ownership(const OrExprNode &node) {
  auto value = value_ownership(*node.expr);
  if (!or_has_handler(node))
    return value;
  return either(value,
                body_ownership(node.fallback.get(), or_result_type(node)));
}

Ownership CodeGen::if_ownership(const IfExprNode &node, const Node &parent) {
  auto result = semantic_type(parent);
  auto *else_body = node.else_block ? node.else_block->get() : nullptr;
  return either(body_ownership(node.then_block.get(), result),
                body_ownership(else_body, result));
}

Ownership CodeGen::switch_ownership(const SwitchExprNode &node,
                                    const Node &parent) {
  auto result = semantic_type(parent);
  auto *else_body = node.else_body ? node.else_body->get() : nullptr;
  auto own = body_ownership(else_body, result);
  for (auto &arm : node.arms)
    own = either(own, body_ownership(arm.body.get(), result));
  return own;
}

void CodeGen::retain_if_borrowed(llvm::Value *val, const TypePtr &sem,
                                 const Node &source) {
  if (val && value_ownership(source) == Ownership::Borrowed)
    emit_retain(val, sem);
}

} // namespace saga

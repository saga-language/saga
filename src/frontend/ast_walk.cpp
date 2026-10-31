// Copyright 2026 Rob Thornton
// SPDX-License-Identifier: MIT

// Walking a body, and what it writes through a binding it names: where `if`,
// `switch` and `for` are expressions, a write can sit inside any operand, so
// the question is answered over the whole tree.

#include "frontend/ast.hpp"

namespace saga {

void for_each_child(const Node &node,
                    const std::function<void(const Node &)> &visit) {
  auto one = [&](const NodePtr &n) {
    if (n)
      visit(*n);
  };
  auto maybe = [&](const std::optional<NodePtr> &n) {
    if (n)
      one(*n);
  };
  auto all = [&](const std::vector<NodePtr> &ns) {
    for (auto &n : ns)
      one(n);
  };
  std::visit(
      overloaded{
          [&](const StringLiteralNode &n) { all(n.fragments); },
          [&](const ArrayLiteralNode &n) { all(n.elements); },
          [&](const MapLiteralNode &n) {
            for (auto &e : n.entries) {
              one(e.key);
              one(e.value);
            }
          },
          [&](const KeyValueNode &n) {
            one(n.key);
            one(n.value);
          },
          [&](const RangeNode &n) {
            one(n.low);
            one(n.high);
          },
          [&](const StructLiteralNode &n) {
            for (auto &f : n.fields)
              one(f.value);
          },
          [&](const FieldAssignmentNode &n) { one(n.value); },
          [&](const CaseArmNode &n) {
            all(n.patterns);
            one(n.body);
          },
          [&](const BinaryExprNode &n) {
            one(n.lhs);
            one(n.rhs);
          },
          [&](const UnaryExprNode &n) { one(n.operand); },
          [&](const IsExpr &n) { one(n.value); },
          [&](const GroupExprNode &n) { one(n.inner); },
          [&](const PromoteExprNode &n) { one(n.operand); },
          [&](const CallExprNode &n) {
            one(n.callee);
            all(n.args);
          },
          [&](const IndexExprNode &n) {
            one(n.object);
            one(n.index);
          },
          [&](const SliceNode &n) {
            maybe(n.low);
            maybe(n.high);
          },
          [&](const SelectorNode &n) { one(n.object); },
          [&](const IfExprNode &n) {
            maybe(n.init);
            one(n.condition);
            one(n.then_block);
            maybe(n.else_block);
          },
          [&](const SwitchExprNode &n) {
            maybe(n.init);
            one(n.subject);
            for (auto &arm : n.arms) {
              all(arm.patterns);
              one(arm.body);
            }
            maybe(n.else_body);
          },
          [&](const ForExprNode &n) {
            maybe(n.mode);
            if (n.accumulator)
              maybe(n.accumulator->init);
            one(n.body);
          },
          [&](const ForRangeClauseNode &n) { one(n.iterable); },
          [&](const ForIterClauseNode &n) {
            one(n.init);
            one(n.condition);
            one(n.update);
          },
          [&](const SpawnExprNode &n) { one(n.body); },
          [&](const OrExprNode &n) {
            one(n.expr);
            one(n.fallback);
          },
          [&](const FuncExprNode &n) { one(n.body); },
          [&](const VarDeclNode &n) { maybe(n.init); },
          [&](const DeclAssignNode &n) { one(n.value); },
          [&](const DestructureNode &n) { one(n.value); },
          [&](const AssignNode &n) {
            all(n.targets);
            all(n.values);
          },
          [&](const IncrementNode &n) { one(n.operand); },
          [&](const DecrementNode &n) { one(n.operand); },
          [&](const ReturnNode &n) { one(n.value); },
          [&](const BreakNode &n) { all(n.values); },
          [&](const BlockNode &n) { all(n.stmts); },
          [&](const auto &) {},
      },
      node.data);
}

std::string_view binding_root(const Node &target) {
  for (const Node *cur = &target; cur;) {
    if (auto *id = std::get_if<IdentifierNode>(&cur->data))
      return id->name;
    if (auto *sel = std::get_if<SelectorNode>(&cur->data)) {
      cur = sel->object.get();
      continue;
    }
    if (auto *idx = std::get_if<IndexExprNode>(&cur->data)) {
      cur = idx->object.get();
      continue;
    }
    break;
  }
  return {};
}

namespace {

// A closure or a task holds a copy of each name it uses, so nothing it does
// reaches the binding outside.
bool holds_own_copies(const Node &node) {
  return std::holds_alternative<FuncExprNode>(node.data) ||
         std::holds_alternative<SpawnExprNode>(node.data);
}

bool writes_directly(const Node &node, std::string_view name) {
  if (auto *n = std::get_if<AssignNode>(&node.data)) {
    for (auto &t : n->targets)
      if (t && binding_root(*t) == name)
        return true;
    return false;
  }
  if (auto *n = std::get_if<IncrementNode>(&node.data))
    return binding_root(*n->operand) == name;
  if (auto *n = std::get_if<DecrementNode>(&node.data))
    return binding_root(*n->operand) == name;
  return false;
}

} // namespace

bool writes_through_binding(const Node &node, std::string_view name) {
  if (name.empty() || holds_own_copies(node))
    return false;
  if (writes_directly(node, name))
    return true;
  bool writes = false;
  for_each_child(node, [&](const Node &child) {
    writes = writes || writes_through_binding(child, name);
  });
  return writes;
}

} // namespace saga

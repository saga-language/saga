// Copywrite 2026 Rob Thornton
// SPDX-License-Identifier: MIT

// Error promotion: `?` resolves the error alternative of a `T | error` so the
// expression can continue as a `T`, and the error it removed travels to the
// enclosing root expression, which puts it back on its own type.

#include "semantic/analyzer.hpp"
#include <format>

namespace saga {

namespace {

std::vector<TypePtr> value_alternatives(const TypePtr &union_type) {
  auto &info = std::get<UnionTypeInfo>(union_type->detail);
  std::vector<TypePtr> values;
  for (auto &alt : info.alternatives) {
    if (!is_error_valued(alt))
      values.push_back(alt);
  }
  return values;
}

std::vector<TypePtr> error_alternatives(const TypePtr &union_type) {
  auto &info = std::get<UnionTypeInfo>(union_type->detail);
  std::vector<TypePtr> errors;
  for (auto &alt : info.alternatives) {
    if (is_error_valued(alt))
      errors.push_back(alt);
  }
  return errors;
}

TypePtr collapse(std::vector<TypePtr> alts) {
  if (alts.size() == 1)
    return alts[0];
  return make_union_type(std::move(alts));
}

} // namespace

TypePtr Analyzer::reject_promotion(Span span, const TypePtr &operand) {
  error(span, std::format("'?' expects a value that can be an error, got {}",
                          type_to_string(operand)));
  return builtins.invalid_type;
}

TypePtr Analyzer::check_promote_expr(const PromoteExprNode &node) {
  auto operand = check_expr(*node.operand);
  if (is_invalid_type(operand))
    return operand;

  if (operand->kind != TypeKind::Union)
    return reject_promotion(node.span, operand);

  auto errors = error_alternatives(operand);
  if (errors.empty())
    return reject_promotion(node.span, operand);

  auto values = value_alternatives(operand);
  if (values.empty()) {
    error(node.span, std::format("'?' leaves no value behind: {} is only an "
                                 "error",
                                 type_to_string(operand)));
    return builtins.invalid_type;
  }

  if (bubble_frames_.empty()) {
    error(node.span, "'?' has nowhere to send the error here");
    return builtins.invalid_type;
  }

  auto &frame = bubble_frames_.back();
  frame.insert(frame.end(), errors.begin(), errors.end());
  return collapse(std::move(values));
}

TypePtr Analyzer::attach_bubbled_errors(TypePtr type) {
  auto errors = std::move(bubble_frames_.back());
  bubble_frames_.pop_back();

  if (errors.empty() || is_invalid_type(type))
    return type;

  std::vector<TypePtr> alts;
  if (type->kind == TypeKind::Union) {
    auto &info = std::get<UnionTypeInfo>(type->detail);
    alts.assign(info.alternatives.begin(), info.alternatives.end());
  } else {
    alts.push_back(std::move(type));
  }

  for (auto &err : errors) {
    bool seen = false;
    for (auto &alt : alts)
      seen = seen || types_equal(alt, err);
    if (!seen)
      alts.push_back(err);
  }
  return collapse(std::move(alts));
}

// `node_types` keeps the type the expression evaluates to on the path where no
// `?` fired, which is what every emitter inside the root reads. The type with
// the errors put back is what the root's *consumer* sees, so it goes in a table
// of its own for codegen to find the landing it has to build.
TypePtr Analyzer::finish_root(const Node &node, TypePtr type) {
  bool promoted = !bubble_frames_.back().empty();
  auto attached = attach_bubbled_errors(std::move(type));
  if (promoted && !is_invalid_type(attached))
    promotion_root_types[&node] = attached;
  return attached;
}

TypePtr Analyzer::check_root_expr(const Node &node) {
  bubble_frames_.emplace_back();
  return finish_root(node, check_expr(node));
}

TypePtr Analyzer::check_root_expr_expecting(const Node &node,
                                            const TypePtr &expected) {
  bubble_frames_.emplace_back();
  return finish_root(node, check_expr_expecting(node, expected));
}

} // namespace saga

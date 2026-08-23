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

  deposit_errors(errors, node.span);
  return collapse(std::move(values));
}

// Outside a receiver chain the error needs no marker: an operand or argument
// that can be an error hands the error onward and the operation proceeds on the
// value. Receivers are the exception because there `?` is what tells the reader
// the rest of the chain may not run.
// A slot that declares it can hold the error is not a place the error is
// escaping from — passing `int | error` to an `int | error` parameter is an
// ordinary argument, not a promotion. Neither is a slot whose type is still
// being inferred: a type parameter takes whatever it is given, so the error
// stays in the value the same way `x := xs[0]` keeps it.
TypePtr Analyzer::bubble_into(const Node &expr, TypePtr type,
                              const TypePtr &expected) {
  if (!expected || has_type_params(expected))
    return type;
  if (is_assignable_to(type, expected))
    return type;
  return bubble_operand(expr, std::move(type));
}

TypePtr Analyzer::bubble_operand(const Node &expr, TypePtr type) {
  if (bubble_frames_.empty() || !type || type->kind != TypeKind::Union)
    return type;

  auto errors = error_alternatives(type);
  auto values = value_alternatives(type);
  if (errors.empty() || values.empty())
    return type;

  deposit_errors(errors, expr.span);
  auto purified = collapse(std::move(values));
  bubbled_operands[&expr] = purified;
  return purified;
}

void Analyzer::deposit_errors(const std::vector<TypePtr> &errors, Span origin) {
  auto &frame = bubble_frames_.back();
  if (frame.errors.empty())
    frame.origin = origin;
  frame.errors.insert(frame.errors.end(), errors.begin(), errors.end());
}

TypePtr Analyzer::attach_bubbled_errors(TypePtr type,
                                        const std::vector<TypePtr> &errors) {
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
//
// A root that evaluates to nothing is the end of the line: a statement has no
// value, so there is nothing for the error to travel in.
TypePtr Analyzer::finish_root(const Node &node, TypePtr type) {
  auto frame = std::move(bubble_frames_.back());
  bubble_frames_.pop_back();

  if (frame.errors.empty() || is_invalid_type(type))
    return type;

  if (!type || type->kind == TypeKind::Void) {
    error(frame.origin, "the error has nowhere to go here; handle it with 'or'");
    return builtins.invalid_type;
  }

  auto attached = attach_bubbled_errors(std::move(type), frame.errors);
  if (!is_invalid_type(attached))
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

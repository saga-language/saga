// Copyright 2026 Rob Thornton
// SPDX-License-Identifier: MIT

// Operators on a type parameter. A bound permits an operator only when every
// type it admits supports it with the same result type, so a body checked once
// against the bound holds for every instantiation.

#include "semantic/analyzer.hpp"

#include <format>

namespace saga {

namespace {

using K = Token::Kind;

// The weakest bound under which the operator is defined at all.
std::optional<TypeConstraint> operator_needs(K op) {
  switch (op) {
  case K::Add:
  case K::Sub:
  case K::Multiply:
  case K::Pow:
  case K::Divide:
  case K::Equal:
  case K::NotEqual:
  case K::LessThan:
  case K::LessThanEqual:
  case K::GreaterThan:
  case K::GreaterThanEqual:
    return TypeConstraint::Numeric;
  case K::Modulo:
  case K::BitwiseAnd:
  case K::BitwiseOr:
  case K::BitwiseXor:
  case K::LeftShift:
  case K::RightShift:
  case K::BitwiseNot:
    return TypeConstraint::Integer;
  default:
    return std::nullopt;
  }
}

bool is_comparison(K op) {
  return op == K::Equal || op == K::NotEqual || op == K::LessThan ||
         op == K::LessThanEqual || op == K::GreaterThan ||
         op == K::GreaterThanEqual;
}

TypeConstraint constraint_of(const TypePtr &param) {
  return std::get<TypeParamInfo>(param->detail).param.constraint;
}

} // namespace

std::optional<TypePtr>
Analyzer::check_type_param_binary(const BinaryExprNode &node,
                                  const TypePtr &lhs, const TypePtr &rhs) {
  auto param = lhs->kind == TypeKind::TypeParam ? lhs : rhs;
  if (!operator_needs(node.op))
    return std::nullopt;
  if (!param_admits_operator(node.op, param, node.span))
    return builtins.invalid_type;
  if (!is_assignable_to(lhs, param) || !is_assignable_to(rhs, param)) {
    error(node.span, std::format("operands of '{}' must both be {}, got {} "
                                 "and {}",
                                 operator_spelling(node.op),
                                 type_to_string(param), type_to_string(lhs),
                                 type_to_string(rhs)));
    return builtins.invalid_type;
  }
  if (is_comparison(node.op))
    return builtins.bool_type;
  if (node.op == K::Divide || node.op == K::Modulo)
    return type_param_quotient(node, param);
  return param;
}

bool Analyzer::param_admits_operator(Token::Kind op, const TypePtr &param,
                                     Span span) {
  auto needs = *operator_needs(op);
  auto has = constraint_of(param);
  if (constraint_within(has, needs))
    return true;
  auto name = type_to_string(param);
  if (has == TypeConstraint::None)
    error(span, std::format("operator '{}' needs a bound on {}: {} {}",
                            operator_spelling(op), name, name,
                            constraint_name(needs)));
  else
    error(span, std::format("operator '{}' needs {} {}, not {} {}",
                            operator_spelling(op), name,
                            constraint_name(needs), name,
                            constraint_name(has)));
  return false;
}

// Integer division has no answer for a zero divisor and float division does,
// so under `numeric` the result type would differ by instantiation.
TypePtr Analyzer::type_param_quotient(const BinaryExprNode &node,
                                      const TypePtr &param) {
  auto has = constraint_of(param);
  if (has == TypeConstraint::Float || divisor_is_known_nonzero(*node.rhs))
    return param;
  if (has == TypeConstraint::Integer)
    return make_union_type({param, builtins.error_base});
  auto name = type_to_string(param);
  error(node.span,
        std::format("'{}' on {} numeric has no single result type: integer "
                    "division can fail and float division cannot; bound {} "
                    "by integer or float, or divide by a non-zero constant",
                    operator_spelling(node.op), name, name));
  return builtins.invalid_type;
}

} // namespace saga

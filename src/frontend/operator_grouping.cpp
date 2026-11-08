// Copyright 2026 Rob Thornton
// SPDX-License-Identifier: MIT

// A precedence exists only where readers already know it or where only one
// grouping can compile. Any other mix has no order and needs parentheses.

#include "frontend/operator_grouping.hpp"

#include <format>

namespace saga {

namespace {

enum class Tier { Other, Arithmetic, Bitwise, Comparison, Logical };

Tier tier_of(Token::Kind op) {
  using K = Token::Kind;
  switch (op) {
  case K::Pow:
  case K::Multiply:
  case K::Divide:
  case K::Modulo:
  case K::Add:
  case K::Sub:
    return Tier::Arithmetic;
  case K::BitwiseAnd:
  case K::BitwiseOr:
  case K::BitwiseXor:
  case K::LeftShift:
  case K::RightShift:
    return Tier::Bitwise;
  case K::Equal:
  case K::NotEqual:
  case K::LessThan:
  case K::LessThanEqual:
  case K::GreaterThan:
  case K::GreaterThanEqual:
  case K::Is:
    return Tier::Comparison;
  case K::LogicalAnd:
  case K::LogicalOr:
    return Tier::Logical;
  default:
    return Tier::Other;
  }
}

std::optional<Token::Kind> operator_of(const Node &operand) {
  if (auto *binary = std::get_if<BinaryExprNode>(&operand.data))
    return binary->op;
  if (std::holds_alternative<IsExpr>(operand.data))
    return Token::Kind::Is;
  return std::nullopt;
}

bool needs_parentheses(Token::Kind parent, Token::Kind child) {
  auto child_tier = tier_of(child);
  switch (tier_of(parent)) {
  case Tier::Comparison:
    return child_tier == Tier::Comparison;
  case Tier::Logical:
    return child_tier == Tier::Logical && child != parent;
  case Tier::Bitwise:
    return child_tier == Tier::Arithmetic ||
           (child_tier == Tier::Bitwise && child != parent);
  default:
    return false;
  }
}

} // namespace

std::optional<std::string> right_operand_grouping_error(Token::Kind op,
                                                        const Node &operand) {
  auto child = operator_of(operand);
  if (!child || !needs_parentheses(op, *child))
    return std::nullopt;
  if (tier_of(op) == Tier::Comparison)
    return "comparisons do not chain; add parentheses";
  return std::format("'{}' and '{}' have no order between them; add "
                     "parentheses",
                     operator_spelling(*child), operator_spelling(op));
}

std::optional<std::string> left_operand_grouping_error(Token::Kind op,
                                                       const Node &operand) {
  auto *unary = std::get_if<UnaryExprNode>(&operand.data);
  if (unary && op == Token::Kind::Pow)
    return std::format("'{}' before '**' could apply to the base or the "
                       "result; add parentheses",
                       operator_spelling(unary->op));
  return right_operand_grouping_error(op, operand);
}

} // namespace saga

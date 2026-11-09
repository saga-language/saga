// Copyright 2026 Rob Thornton
// SPDX-License-Identifier: MIT

#include "frontend/ast.hpp"
#include "frontend/file.hpp"
#include "frontend/fileset.hpp"
#include "frontend/parser.hpp"

#include <gtest/gtest.h>

#include <string>

namespace saga {
namespace {

std::string grouped(const Node &node);

std::string grouped_binary(const BinaryExprNode &binary) {
  return "(" + grouped(*binary.lhs) + " " +
         std::string(operator_spelling(binary.op)) + " " +
         grouped(*binary.rhs) + ")";
}

std::string grouped_unary(const UnaryExprNode &unary) {
  return "(" + std::string(operator_spelling(unary.op)) +
         grouped(*unary.operand) + ")";
}

// Every test row tests against the type T, so the type's spelling is not read.
std::string grouped_is(const IsExpr &is) {
  return "(" + grouped(*is.value) + " is T)";
}

std::string grouped(const Node &node) {
  if (auto *binary = std::get_if<BinaryExprNode>(&node.data))
    return grouped_binary(*binary);
  if (auto *unary = std::get_if<UnaryExprNode>(&node.data))
    return grouped_unary(*unary);
  if (auto *is = std::get_if<IsExpr>(&node.data))
    return grouped_is(*is);
  if (auto *group = std::get_if<GroupExprNode>(&node.data))
    return grouped(*group->inner);
  return std::string(std::get<IdentifierNode>(node.data).name);
}

struct Row {
  const char *source;
  const char *expected;
};

void expect_grouping(const Row &row) {
  SCOPED_TRACE(row.source);
  FileSet fs;
  fs.add_file(File::from_source("test.sg", row.source));
  Parser parser(fs);
  parser.init_for_file(fs.files[0].get());
  NodePtr expr = parser.parse_expression();
  ASSERT_TRUE(parser.errors.errors.empty());
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(grouped(*expr), row.expected);
}

TEST(OperatorGrouping, Logical) {
  const Row rows[] = {
      {"a || b && c", "(a || (b && c))"},
      {"a && b || c", "((a && b) || c)"},
      {"a && b || c && d", "((a && b) || (c && d))"},
      {"(a || b) && c", "((a || b) && c)"},
      {"a || b || c", "((a || b) || c)"},
      {"a && b && c", "((a && b) && c)"},
  };
  for (const Row &row : rows)
    expect_grouping(row);
}

TEST(OperatorGrouping, Comparison) {
  const Row rows[] = {
      {"a && b == c", "(a && (b == c))"},
      {"a == b && c", "((a == b) && c)"},
      {"a < b && c > d", "((a < b) && (c > d))"},
      {"a && b is T", "(a && (b is T))"},
      {"a is T && b", "((a is T) && b)"},
      {"a == b == c", "((a == b) == c)"},
      {"a < b == c", "((a < b) == c)"},
      {"a == b is T", "((a == b) is T)"},
      {"a is T == b", "((a is T) == b)"},
      {"a is T is T", "((a is T) is T)"},
      {"a + b is T", "((a + b) is T)"},
      {"a & b == c", "((a & b) == c)"},
      {"a | b == c", "((a | b) == c)"},
  };
  for (const Row &row : rows)
    expect_grouping(row);
}

TEST(OperatorGrouping, Bitwise) {
  const Row rows[] = {
      {"a | b & c", "(a | (b & c))"},
      {"a & b | c", "((a & b) | c)"},
      {"a ^ b & c", "(a ^ (b & c))"},
      {"a | b ^ c", "((a | b) ^ c)"},
      {"a << b & c", "((a << b) & c)"},
      {"a & b << c", "((a & b) << c)"},
      {"a << b >> c", "((a << b) >> c)"},
      {"a << b | c", "((a << b) | c)"},
      {"a | b << c", "(a | (b << c))"},
  };
  for (const Row &row : rows)
    expect_grouping(row);
}

TEST(OperatorGrouping, BitwiseWithArithmetic) {
  const Row rows[] = {
      {"a << b + c", "((a << b) + c)"},
      {"a + b << c", "(a + (b << c))"},
      {"a + b & c", "(a + (b & c))"},
      {"a & b + c", "((a & b) + c)"},
      {"a * b | c", "((a * b) | c)"},
      {"a | b * c", "(a | (b * c))"},
      {"a + b | c", "((a + b) | c)"},
      {"a | b + c", "((a | b) + c)"},
      {"a * b & c", "((a * b) & c)"},
      {"a & b * c", "((a & b) * c)"},
      {"a ** b & c", "((a ** b) & c)"},
  };
  for (const Row &row : rows)
    expect_grouping(row);
}

TEST(OperatorGrouping, Unary) {
  const Row rows[] = {
      {"-a ** b", "(-(a ** b))"},
      {"!a ** b", "(!(a ** b))"},
      {"~a ** b", "(~(a ** b))"},
      {"(-a) ** b", "((-a) ** b)"},
      {"a ** -b", "(a ** (-b))"},
      {"a ** -b ** c", "(a ** (-(b ** c)))"},
      {"a ** -b * c", "((a ** (-b)) * c)"},
      {"-a * b", "((-a) * b)"},
      {"~a & b", "((~a) & b)"},
      {"!a == b", "((!a) == b)"},
      {"!a && b", "((!a) && b)"},
  };
  for (const Row &row : rows)
    expect_grouping(row);
}

TEST(OperatorGrouping, Arithmetic) {
  const Row rows[] = {
      {"a + b * c", "(a + (b * c))"},
      {"a * b ** c", "(a * (b ** c))"},
      {"a ** b ** c", "(a ** (b ** c))"},
      {"a - b + c", "((a - b) + c)"},
      {"a * b % c", "((a * b) % c)"},
  };
  for (const Row &row : rows)
    expect_grouping(row);
}

} // namespace
} // namespace saga

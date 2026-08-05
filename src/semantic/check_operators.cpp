// Copywrite 2026 Rob Thornton
// SPDX-License-Identifier: MIT

// Operators: the arithmetic and comparison rules, and the struct overloads a
// user type can supply for them. An overload is found by method name, so a
// missing one has to be reported as the operator the source actually wrote.

#include "semantic/analyzer.hpp"
#include <format>

namespace saga {

TypePtr Analyzer::check_struct_binary_expr(const BinaryExprNode &node,
                                            const Node &parent,
                                            const TypePtr &lhs,
                                            const TypePtr &rhs) {
  auto &info = std::get<StructTypeInfo>(lhs->detail);

  // Helper: returns true if the struct declares a method with the given name.
  auto has_method = [&](const std::string &name) -> bool {
    for (auto &m : info.methods)
      if (m.name == name)
        return true;
    return false;
  };

  // Helper: record the resolved method and return the result type.
  auto resolve = [&](const std::string &method, TypePtr result) -> TypePtr {
    if (current_instantiation_) {
      current_instantiation_->struct_operator_methods[&parent] = method;
    } else {
      struct_operator_methods[&parent] = method;
    }
    return result;
  };

  using K = Token::Kind;

  switch (node.op) {
  // ── Additive ───────────────────────────────────────────────────────────────
  case K::Add:
    if (has_method("Add")) {
      expect_assignable(node.rhs->span, lhs, rhs, "Add argument");
      return resolve("Add", lhs);
    }
    error(node.span,
          std::format("type {} does not implement Adder (no Add method)",
                      type_to_string(lhs)));
    return builtins.invalid_type;

  case K::Sub:
    if (has_method("Sub")) {
      expect_assignable(node.rhs->span, lhs, rhs, "Sub argument");
      return resolve("Sub", lhs);
    }
    error(node.span,
          std::format("type {} does not implement Subber (no Sub method)",
                      type_to_string(lhs)));
    return builtins.invalid_type;

  // ── Multiplicative ───────────────────────────────────────────────────────
  case K::Multiply:
    if (has_method("Mul")) {
      expect_assignable(node.rhs->span, lhs, rhs, "Mul argument");
      return resolve("Mul", lhs);
    }
    error(node.span,
          std::format("type {} does not implement Multiplier (no Mul method)",
                      type_to_string(lhs)));
    return builtins.invalid_type;

  case K::Divide:
    if (has_method("Div")) {
      expect_assignable(node.rhs->span, lhs, rhs, "Div argument");
      // Divisable returns T | Error (can fail, e.g. divide by zero).
      return resolve("Div", make_union_type({lhs, builtins.error_base}));
    }
    error(node.span,
          std::format("type {} does not implement Divisable (no Div method)",
                      type_to_string(lhs)));
    return builtins.invalid_type;

  // ── Equality ──────────────────────────────────────────────────────────────
  case K::Equal:
  case K::NotEqual:
    // Prefer Equals (runtime convention), then Equal (interface name),
    // then Compare as a fallback (Comparison.Equal == 1).
    if (has_method("Equals")) {
      expect_assignable(node.rhs->span, lhs, rhs, "Equals argument");
      return resolve("Equals", builtins.bool_type);
    }
    if (has_method("Equal")) {
      expect_assignable(node.rhs->span, lhs, rhs, "Equal argument");
      return resolve("Equal", builtins.bool_type);
    }
    if (has_method("Compare")) {
      // Fall back: Compare() == Comparison.Equal (1) → Bool.
      expect_assignable(node.rhs->span, lhs, rhs, "Compare argument");
      return resolve("Compare", builtins.bool_type);
    }
    error(node.span,
          std::format("type {} does not support equality (no Equals, Equal, "
                      "or Compare method)",
                      type_to_string(lhs)));
    return builtins.invalid_type;

  // ── Ordering ──────────────────────────────────────────────────────────────
  case K::LessThan:
  case K::LessThanEqual:
  case K::GreaterThan:
  case K::GreaterThanEqual:
    if (has_method("Compare")) {
      expect_assignable(node.rhs->span, lhs, rhs, "Compare argument");
      return resolve("Compare", builtins.bool_type);
    }
    error(node.span,
          std::format("type {} does not implement Comparable (no Compare "
                      "method)",
                      type_to_string(lhs)));
    return builtins.invalid_type;

  default:
    error(node.span,
          std::format("operator not supported for type {}",
                      type_to_string(lhs)));
    return builtins.invalid_type;
  }
}

TypePtr Analyzer::check_binary_expr(const BinaryExprNode &node,
                                    const Node &parent) {
  // A `.Variant` operand (an enum comparison, `c == .Red`) resolves against the
  // other operand's enum type; the other side is checked first to supply it.
  bool lhs_sh = std::get_if<EnumShorthandNode>(&node.lhs->data) != nullptr;
  bool rhs_sh = std::get_if<EnumShorthandNode>(&node.rhs->data) != nullptr;
  TypePtr lhs, rhs;
  if (rhs_sh && !lhs_sh) {
    lhs = check_expr(*node.lhs);
    rhs = check_expr_expecting(*node.rhs, lhs);
  } else if (lhs_sh && !rhs_sh) {
    rhs = check_expr(*node.rhs);
    lhs = check_expr_expecting(*node.lhs, rhs);
  } else {
    lhs = check_expr(*node.lhs);
    rhs = check_expr(*node.rhs);
  }

  if (is_invalid_type(lhs) || is_invalid_type(rhs))
    return builtins.invalid_type;

  // Errors compare by value: `==`/`!=` on two errors is structural (same type
  // and equal fields). Errors have no methods, so they never reach the struct
  // operator-overload path below. Structural comparison needs a concrete layout,
  // so at least one side must have a concrete error type — comparing two base
  // `error` values can't see their extra fields and is rejected.
  using EK = Token::Kind;
  if ((node.op == EK::Equal || node.op == EK::NotEqual) &&
      is_error_valued(lhs) && is_error_valued(rhs)) {
    if (is_abstract_error(lhs) && is_abstract_error(rhs))
      error(node.span,
            "cannot compare two base `error` values; at least one side must "
            "have a concrete error type (narrow with `is` first)");
    return builtins.bool_type;
  }

  // Enums and errors are identity types: they carry no arithmetic and cannot
  // overload it. Reject arithmetic here — errors would otherwise reach the
  // struct operator-overload path and be told they lack an `Add` method they
  // can never define (methods cannot be attached to error types).
  switch (node.op) {
  case EK::Add:
  case EK::Sub:
  case EK::Multiply:
  case EK::Divide:
  case EK::Pow:
  case EK::Modulo:
    for (auto &operand : {lhs, rhs}) {
      if (is_enum_valued(operand) || is_error_valued(operand)) {
        error(node.span,
              std::format("type {} does not support arithmetic operators",
                          type_to_string(operand)));
        return builtins.invalid_type;
      }
    }
    break;
  default:
    break;
  }

  // ── Struct operator overloading ──────────────────────────────────────────
  // Dispatch to method-based overloading before the built-in numeric/string
  // paths, so user types can override operators on structs.
  if (lhs->kind == TypeKind::Struct) {
    return check_struct_binary_expr(node, parent, lhs, rhs);
  }

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
      return builtins.invalid_type;
    }
    if (!is_numeric(rhs)) {
      error(node.rhs->span,
            std::format("arithmetic operator requires numeric type, got {}",
                        type_to_string(rhs)));
      return builtins.invalid_type;
    }
    return common_type(lhs, rhs);
  }

  // Division: returns T | Error (division by zero).
  case K::Divide: {
    if (!is_numeric(lhs) || !is_numeric(rhs)) {
      error(node.span,
            std::format("division requires numeric types, got {} and {}",
                        type_to_string(lhs), type_to_string(rhs)));
      return builtins.invalid_type;
    }
    auto result = common_type(lhs, rhs);
    return make_union_type({result, builtins.error_base});
  }

  // Comparison: == != > < >= <=
  case K::Equal:
  case K::NotEqual: {
    // Type tests are spelled `value is Type` (see check_is_expr), not `==`.
    if (!is_equatable(lhs)) {
      error(node.lhs->span, std::format("type {} does not support equality",
                                        type_to_string(lhs)));
      return builtins.invalid_type;
    }
    expect_assignable(node.rhs->span, lhs, rhs, "comparison");
    return builtins.bool_type;
  }
  case K::LessThan:
  case K::LessThanEqual:
  case K::GreaterThan:
  case K::GreaterThanEqual: {
    if (!is_ordered(lhs)) {
      error(node.lhs->span, std::format("type {} does not support ordering",
                                        type_to_string(lhs)));
      return builtins.invalid_type;
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
      return builtins.invalid_type;
    }
    if (rhs->kind != TypeKind::Int) {
      error(node.rhs->span,
            std::format("bitwise operator requires integer type, got {}",
                        type_to_string(rhs)));
      return builtins.invalid_type;
    }
    return common_type(lhs, rhs);
  }

  default:
    error(node.span, "unsupported binary operator");
    return builtins.invalid_type;
  }
}

TypePtr Analyzer::check_unary_expr(const UnaryExprNode &node) {
  auto operand = check_expr(*node.operand);
  if (is_invalid_type(operand))
    return builtins.invalid_type;

  if (node.op == Token::Kind::Not) {
    expect_bool(node.operand->span, operand, "logical not");
    return builtins.bool_type;
  }
  if (node.op == Token::Kind::Sub) {
    if (is_enum_valued(operand) || is_error_valued(operand)) {
      error(node.operand->span,
            std::format("type {} does not support arithmetic operators",
                        type_to_string(operand)));
      return builtins.invalid_type;
    }
    if (!is_numeric(operand)) {
      error(node.operand->span,
            std::format("negation requires numeric type, got {}",
                        type_to_string(operand)));
      return builtins.invalid_type;
    }
    return operand;
  }
  if (node.op == Token::Kind::BitwiseNot) {
    if (operand->kind != TypeKind::Int) {
      error(node.operand->span,
            std::format("bitwise NOT requires integer type, got {}",
                        type_to_string(operand)));
      return builtins.invalid_type;
    }
    return operand;
  }

  error(node.span, "unsupported unary operator");
  return builtins.invalid_type;
}

TypePtr Analyzer::check_is_expr(const IsExpr &node) {
  auto value_type = check_expr(*node.value);
  auto test_type = resolve_type(*node.type);
  record_type(*node.type, test_type);

  if (is_invalid_type(value_type) || is_invalid_type(test_type))
    return builtins.bool_type;

  if (value_type->kind == TypeKind::Union) {
    auto &info = std::get<UnionTypeInfo>(value_type->detail);
    bool found = false;
    for (auto &alt : info.alternatives) {
      if (types_equal(alt, test_type) || is_assignable_to(test_type, alt)) {
        found = true;
        break;
      }
    }
    if (!found)
      error(node.type->span,
            std::format("type {} is not an alternative of {}",
                        type_to_string(test_type), type_to_string(value_type)));
  }

  return builtins.bool_type;
}

TypePtr Analyzer::check_group_expr(const GroupExprNode &node) {
  return check_expr(*node.inner);
}

} // namespace saga

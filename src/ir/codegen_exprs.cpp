// Copyright 2026 Rob Thornton
// SPDX-License-Identifier: MIT

#include "ir/codegen.hpp"
#include "util/internal_error.hpp"

#include <llvm/IR/Constants.h>
#include <llvm/IR/GlobalVariable.h>

#include <charconv>

namespace saga {

// ===========================================================================
// Expression emission
// ===========================================================================

llvm::Value *CodeGen::emit_expr(const Node &node) {
  return std::visit(
      overloaded{
          [&](const IntegerLiteralNode &n) -> llvm::Value * {
            return emit_int_literal(n);
          },
          [&](const FloatLiteralNode &n) -> llvm::Value * {
            return emit_float_literal(n);
          },
          [&](const BoolLiteralNode &n) -> llvm::Value * {
            return emit_bool_literal(n);
          },
          [&](const EnumShorthandNode &n) -> llvm::Value * {
            return emit_enum_shorthand(n, node);
          },
          [&](const StringLiteralNode &n) -> llvm::Value * {
            return emit_string_literal(n);
          },
          [&](const BinaryExprNode &n) -> llvm::Value * {
            return emit_binary_expr(n, node);
          },
          [&](const UnaryExprNode &n) -> llvm::Value * {
            return emit_unary_expr(n);
          },
          [&](const IsExpr &n) -> llvm::Value * { return emit_is_expr(n); },
          [&](const GroupExprNode &n) -> llvm::Value * {
            return emit_group_expr(n);
          },
          [&](const IfExprNode &n) -> llvm::Value * {
            return emit_if_expr(n, node);
          },
          [&](const ForExprNode &n) -> llvm::Value * {
            return emit_for_expr(n, node);
          },
          [&](const SwitchExprNode &n) -> llvm::Value * {
            return emit_switch_expr(n, node);
          },
          [&](const StructLiteralNode &n) -> llvm::Value * {
            return emit_struct_literal(n, node);
          },
          [&](const SelectorNode &n) -> llvm::Value * {
            return emit_selector(n, node);
          },
          [&](const ArrayLiteralNode &n) -> llvm::Value * {
            return emit_array_literal(n, node);
          },
          [&](const RangeNode &n) -> llvm::Value * {
            return emit_range_literal(n);
          },
          [&](const MapLiteralNode &n) -> llvm::Value * {
            return emit_map_literal(n, node);
          },
          [&](const IndexExprNode &n) -> llvm::Value * {
            return emit_index_expr(n);
          },
          [&](const BreakNode &n) -> llvm::Value * {
            emit_break(n);
            return nullptr;
          },
          [&](const NextNode &) -> llvm::Value * {
            emit_next();
            return nullptr;
          },
          [&](const OrExprNode &n) -> llvm::Value * {
            return emit_or_expr(n);
          },
          [&](const CallExprNode &n) -> llvm::Value * {
            return emit_call_expr(n, node);
          },
          [&](const FuncExprNode &n) -> llvm::Value * {
            return emit_func_expr(n, node);
          },
          [&](const SpawnExprNode &n) -> llvm::Value * {
            return emit_spawn_expr(n, node);
          },
          [&](const IdentifierNode &n) -> llvm::Value * {
            return emit_identifier(n, node);
          },
          [&](const VarDeclNode &n) -> llvm::Value * {
            emit_var_decl(n);
            return nullptr;
          },
          [&](const DeclAssignNode &n) -> llvm::Value * {
            emit_decl_assign(n);
            return nullptr;
          },
          [&](const DestructureNode &n) -> llvm::Value * {
            emit_destructure(n);
            return nullptr;
          },
          [&](const AssignNode &n) -> llvm::Value * {
            emit_assign(n);
            return nullptr;
          },
          [&](const ReturnNode &n) -> llvm::Value * {
            emit_return(n);
            return nullptr;
          },
          [&](const IncrementNode &n) -> llvm::Value * {
            emit_increment(n);
            return nullptr;
          },
          [&](const DecrementNode &n) -> llvm::Value * {
            emit_decrement(n);
            return nullptr;
          },
          [&](const PromoteExprNode &n) -> llvm::Value * {
            return emit_promote_expr(n);
          },
          [&](const auto &) -> llvm::Value * {
            return nullptr;
          },
      },
      node.data);
}

// ===========================================================================
// Literal emission
// ===========================================================================

/// Parse an integer literal string, handling 0b, 0o, 0x prefixes and
/// underscore separators.
static int64_t parse_int_literal(std::string_view lit) {
  // Strip underscores.
  std::string clean;
  clean.reserve(lit.size());
  for (char c : lit) {
    if (c != '_')
      clean += c;
  }

  int base = 10;
  std::string_view digits = clean;
  if (digits.size() > 2 && digits[0] == '0') {
    switch (digits[1]) {
    case 'b': case 'B': base = 2;  digits = digits.substr(2); break;
    case 'o': case 'O': base = 8;  digits = digits.substr(2); break;
    case 'x': case 'X': base = 16; digits = digits.substr(2); break;
    default: break;
    }
  }

  int64_t val = 0;
  std::from_chars(digits.data(), digits.data() + digits.size(), val, base);
  return val;
}

static double parse_float_literal(std::string_view lit) {
  std::string clean;
  clean.reserve(lit.size());
  for (char c : lit) {
    if (c != '_')
      clean += c;
  }
  double val = 0.0;
  std::from_chars(clean.data(), clean.data() + clean.size(), val);
  return val;
}

llvm::Value *CodeGen::emit_int_literal(const IntegerLiteralNode &node) {
  int64_t val = parse_int_literal(node.literal);
  return llvm::ConstantInt::get(i64_type, static_cast<uint64_t>(val),
                                /*isSigned=*/true);
}

llvm::Value *CodeGen::emit_float_literal(const FloatLiteralNode &node) {
  double val = parse_float_literal(node.literal);
  return llvm::ConstantFP::get(f64_type, val);
}

llvm::Value *CodeGen::emit_bool_literal(const BoolLiteralNode &node) {
  bool val = (node.literal == "true");
  return llvm::ConstantInt::get(i1_type, val ? 1 : 0);
}

// `.Variant` shorthand — the analyzer recorded the resolved enum type on the
// node; emit the variant's ordinal constant, identical to EnumName.Variant.
llvm::Value *CodeGen::emit_enum_shorthand(const EnumShorthandNode &n,
                                          const Node &node) {
  auto sem = semantic_type(node);
  if (!sem || sem->kind != TypeKind::Enum)
    return nullptr;
  auto &info = std::get<EnumTypeInfo>(sem->detail);
  std::string ev_key =
      key_for(info.origin_package, info.name) + "." + std::string(n.variant.name);
  auto it = enum_variants.find(ev_key);
  if (it != enum_variants.end())
    return llvm::ConstantInt::get(i64_type, it->second);
  return nullptr;
}

// ===========================================================================
// Semantic type query
// ===========================================================================

TypePtr CodeGen::declared_type(const Node &node) const {
  if (current_instantiation_) {
    auto it = current_instantiation_->node_types.find(&node);
    if (it != current_instantiation_->node_types.end())
      return it->second;
  }
  auto it = analyzer.node_types.find(&node);
  return it != analyzer.node_types.end() ? it->second : nullptr;
}

TypePtr CodeGen::semantic_type(const Node &node) const {
  return unwrap_alias(declared_type(node));
}

TypePtr CodeGen::block_result_type(const BlockNode &block) const {
  return block.stmts.empty() ? nullptr : root_expr_type(*block.stmts.back());
}

TypePtr CodeGen::body_result_type(const Node &body) const {
  if (auto *block = std::get_if<BlockNode>(&body.data))
    return block_result_type(*block);
  return semantic_type(body);
}

// ---------------------------------------------------------------------------
// Per-instantiation side-table accessors (monomorphism_plan.md, Step 4)
//
// Each accessor checks current_instantiation_'s per-instantiation view
// first, then falls through to the corresponding analyzer.<table> global.
// A miss in the instantiation view is NOT an error — nodes outside the
// generic body live only in the global tables.
// ---------------------------------------------------------------------------

const Symbol *CodeGen::node_symbol(const Node &node) const {
  if (current_instantiation_) {
    auto it = current_instantiation_->node_symbols.find(&node);
    if (it != current_instantiation_->node_symbols.end())
      return &it->second;
  }
  auto it = analyzer.node_symbols.find(&node);
  if (it != analyzer.node_symbols.end())
    return &it->second;
  return nullptr;
}

const std::vector<Analyzer::CaptureInfo> *
CodeGen::node_captures_of(const Node &node) const {
  if (current_instantiation_) {
    auto it = current_instantiation_->node_captures.find(&node);
    if (it != current_instantiation_->node_captures.end())
      return &it->second;
  }
  auto it = analyzer.node_captures.find(&node);
  if (it != analyzer.node_captures.end())
    return &it->second;
  return nullptr;
}

const std::vector<Analyzer::SpawnCaptureInfo> *
CodeGen::spawn_captures_of(const Node &node) const {
  if (current_instantiation_) {
    auto it = current_instantiation_->spawn_captures.find(&node);
    if (it != current_instantiation_->spawn_captures.end())
      return &it->second;
  }
  auto it = analyzer.spawn_captures.find(&node);
  if (it != analyzer.spawn_captures.end())
    return &it->second;
  return nullptr;
}

TypePtr CodeGen::iterable_next_elem_type_of(const Node &node) const {
  if (current_instantiation_) {
    auto it = current_instantiation_->iterable_next_elem_type.find(&node);
    if (it != current_instantiation_->iterable_next_elem_type.end())
      return it->second;
  }
  auto it = analyzer.iterable_next_elem_type.find(&node);
  if (it != analyzer.iterable_next_elem_type.end())
    return it->second;
  return nullptr;
}

TypePtr CodeGen::spawn_channel_elem_type_of(const Node &node) const {
  if (current_instantiation_) {
    auto it = current_instantiation_->spawn_channel_elem_types.find(&node);
    if (it != current_instantiation_->spawn_channel_elem_types.end())
      return it->second;
  }
  auto it = analyzer.spawn_channel_elem_types.find(&node);
  if (it != analyzer.spawn_channel_elem_types.end())
    return it->second;
  return nullptr;
}

const std::string *
CodeGen::struct_operator_method_of(const Node &node) const {
  if (current_instantiation_) {
    auto it = current_instantiation_->struct_operator_methods.find(&node);
    if (it != current_instantiation_->struct_operator_methods.end())
      return &it->second;
  }
  auto it = analyzer.struct_operator_methods.find(&node);
  if (it != analyzer.struct_operator_methods.end())
    return &it->second;
  return nullptr;
}

TypePtr CodeGen::span_type_at(Span span) const {
  auto scan = [&](const std::vector<std::pair<Span, TypePtr>> &v) -> TypePtr {
    for (auto &[s, t] : v) {
      if (s.start == span.start && s.end == span.end)
        return t;
    }
    return nullptr;
  };
  if (current_instantiation_) {
    if (auto t = scan(current_instantiation_->span_types))
      return t;
  }
  return scan(analyzer.span_types);
}

const std::unordered_map<uint32_t, TypePtr> *
CodeGen::node_type_args_of(const Node &node) const {
  if (current_instantiation_) {
    auto it = current_instantiation_->node_type_args.find(&node);
    if (it != current_instantiation_->node_type_args.end())
      return &it->second;
  }
  auto it = analyzer.node_type_args.find(&node);
  if (it != analyzer.node_type_args.end())
    return &it->second;
  return nullptr;
}

std::string CodeGen::struct_method_link_name(const StructTypeInfo &info,
                                             const std::string &method) {
  auto ml_it = struct_method_links.find(info.name);
  if (ml_it != struct_method_links.end())
    for (auto &[lname, mname] : ml_it->second)
      if (mname == method)
        return lname;
  return mangle(info.name + "__" + method);
}

// ===========================================================================
// Unary expressions
// ===========================================================================

llvm::Value *CodeGen::emit_unary_expr(const UnaryExprNode &node) {
  auto *operand = emit_expr(*node.operand);
  if (!operand)
    return nullptr;

  using K = Token::Kind;
  switch (node.op) {
  case K::Sub:
    if (operand->getType()->isDoubleTy())
      return builder.CreateFNeg(operand, "fneg");
    return builder.CreateNeg(operand, "neg");
  case K::Not:
    return builder.CreateNot(operand, "not");
  case K::BitwiseNot:
    return builder.CreateNot(operand, "bnot");
  default:
    return nullptr;
  }
}

// emit_is_expr — `value is Type`. For a union operand, load and compare the tag
// byte (the analyzer guarantees Type is an alternative). For any other operand
// the result is a compile-time constant: true iff the static type matches.
llvm::Value *CodeGen::emit_is_expr(const IsExpr &node) {
  auto value_sem = semantic_type(*node.value);
  auto test_sem = semantic_type(*node.type);
  auto *i1 = llvm::Type::getInt1Ty(context);

  if (value_sem && is_error_valued(value_sem) && test_sem &&
      is_error_valued(test_sem))
    return emit_error_is(node, value_sem, test_sem);

  if (value_sem && value_sem->kind == TypeKind::Union)
    return emit_union_is(node, value_sem, test_sem);

  emit_borrowed(*node.value); // evaluated for its effects, then folded
  bool same = value_sem && test_sem && types_equal(value_sem, test_sem);
  return llvm::ConstantInt::get(i1, same ? 1 : 0);
}

// `err is SomeError` on an error-typed value. Testing against base `error` is
// true for any error; a concrete-typed value folds statically; a base-`error`
// value compares the box's runtime type_id (offset 0) against the tested type.
llvm::Value *CodeGen::emit_error_is(const IsExpr &node, const TypePtr &value_sem,
                                    const TypePtr &test_sem) {
  auto *i1 = llvm::Type::getInt1Ty(context);
  auto *box = emit_borrowed(*node.value);
  if (!box)
    return nullptr;
  if (is_abstract_error(test_sem))
    return llvm::ConstantInt::get(i1, 1);
  if (!is_abstract_error(value_sem))
    return llvm::ConstantInt::get(i1,
                                  types_equal(value_sem, test_sem) ? 1 : 0);
  auto &tinfo = std::get<StructTypeInfo>(unwrap_alias(test_sem)->detail);
  return builder.CreateICmpEQ(
      builder.CreateLoad(i64_type, box, "is.tid"),
      llvm::ConstantInt::get(i64_type, error_type_id(tinfo)), "is.type");
}

// `x is T` on a union value: the tag identifies the alternative. When the
// matched slot is the base-`error` slot but T is a concrete error, also compare
// the boxed type_id so `x is NetworkError` distinguishes it from other errors.
llvm::Value *CodeGen::emit_union_is(const IsExpr &node, const TypePtr &value_sem,
                                    const TypePtr &test_sem) {
  auto *i1 = llvm::Type::getInt1Ty(context);
  auto *i8 = llvm::Type::getInt8Ty(context);
  int tag = union_tag_for_type(test_sem, value_sem);
  auto *union_val = emit_borrowed(*node.value);
  if (!union_val)
    return nullptr;
  if (tag < 0)
    return llvm::ConstantInt::get(i1, 0);

  auto *union_st = get_union_llvm_type(value_sem);
  auto *tag_gep = builder.CreateStructGEP(union_st, union_val, 0, "is.tag.ptr");
  auto *tag_ok = builder.CreateICmpEQ(
      builder.CreateLoad(i8, tag_gep, "is.tag"),
      llvm::ConstantInt::get(i8, tag), "is.eq");

  auto &info = std::get<UnionTypeInfo>(value_sem->detail);
  bool concrete_in_error_slot = is_abstract_error(info.alternatives[tag]) &&
                                is_error_valued(test_sem) &&
                                !is_abstract_error(test_sem);
  if (!concrete_in_error_slot)
    return tag_ok;

  auto *payload_gep =
      builder.CreateStructGEP(union_st, union_val, 1, "is.box.ptr");
  auto *box = builder.CreateLoad(llvm::PointerType::getUnqual(context),
                                 payload_gep, "is.box");
  auto &tinfo = std::get<StructTypeInfo>(unwrap_alias(test_sem)->detail);
  auto *tid_ok = builder.CreateICmpEQ(
      builder.CreateLoad(i64_type, box, "is.tid"),
      llvm::ConstantInt::get(i64_type, error_type_id(tinfo)), "is.type");
  return builder.CreateAnd(tag_ok, tid_ok, "is.slot.type");
}

} // namespace saga

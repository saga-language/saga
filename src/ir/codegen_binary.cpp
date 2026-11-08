// Copyright 2026 Rob Thornton
// SPDX-License-Identifier: MIT

#include "ir/codegen.hpp"

#include <llvm/IR/Constants.h>
#include <llvm/IR/Intrinsics.h>

#include <optional>

namespace saga {

namespace {

struct Comparison {
  llvm::CmpInst::Predicate int_predicate;
  llvm::CmpInst::Predicate float_predicate;
  const char *name;
};

std::optional<Comparison> comparison_of(Token::Kind op) {
  using K = Token::Kind;
  using P = llvm::CmpInst::Predicate;
  switch (op) {
  case K::Equal:
    return Comparison{P::ICMP_EQ, P::FCMP_OEQ, "eq"};
  case K::NotEqual:
    return Comparison{P::ICMP_NE, P::FCMP_UNE, "ne"};
  case K::LessThan:
    return Comparison{P::ICMP_SLT, P::FCMP_OLT, "lt"};
  case K::LessThanEqual:
    return Comparison{P::ICMP_SLE, P::FCMP_OLE, "le"};
  case K::GreaterThan:
    return Comparison{P::ICMP_SGT, P::FCMP_OGT, "gt"};
  case K::GreaterThanEqual:
    return Comparison{P::ICMP_SGE, P::FCMP_OGE, "ge"};
  default:
    return std::nullopt;
  }
}

bool compares_errors(Token::Kind op, const TypePtr &lhs, const TypePtr &rhs) {
  bool is_equality = op == Token::Kind::Equal || op == Token::Kind::NotEqual;
  return is_equality && lhs && rhs && is_error_valued(lhs) &&
         is_error_valued(rhs);
}

const FuncTypeInfo *operator_method_signature(const StructTypeInfo &info,
                                              const std::string &method) {
  for (auto &m : info.methods) {
    if (m.name != method)
      continue;
    if (m.signature && m.signature->kind == TypeKind::Func)
      return &std::get<FuncTypeInfo>(m.signature->detail);
    return nullptr;
  }
  return nullptr;
}

} // namespace

llvm::Value *CodeGen::emit_binary_expr(const BinaryExprNode &node,
                                       const Node &parent) {
  if (node.op == Token::Kind::LogicalAnd || node.op == Token::Kind::LogicalOr)
    return emit_logical_expr(node);
  auto lhs_sem = operand_type(*node.lhs);
  auto rhs_sem = operand_type(*node.rhs);
  if (compares_errors(node.op, lhs_sem, rhs_sem))
    return emit_error_equality(node, lhs_sem, rhs_sem);
  if (lhs_sem && lhs_sem->kind == TypeKind::Struct)
    if (auto *method = struct_operator_method_of(parent))
      return emit_struct_binary_op(node, lhs_sem, *method);

  auto *lhs = emit_borrowed_operand(*node.lhs);
  auto *rhs = emit_borrowed_operand(*node.rhs);
  if (!lhs || !rhs)
    return nullptr;
  if (lhs_sem && lhs_sem->kind == TypeKind::String)
    return emit_string_op(node.op, lhs, rhs);
  return emit_numeric_op(node.op, lhs, rhs, parent);
}

llvm::Value *CodeGen::emit_string_op(Token::Kind op, llvm::Value *lhs,
                                     llvm::Value *rhs) {
  if (op == Token::Kind::Add)
    return builder.CreateCall(module->getFunction("saga_string_concat"),
                              {lhs, rhs}, "concat");
  auto comparison = comparison_of(op);
  if (!comparison)
    return nullptr;
  auto *order = builder.CreateCall(module->getFunction("saga_string_compare"),
                                   {lhs, rhs}, "strcmp");
  return builder.CreateICmp(comparison->int_predicate, order,
                            llvm::ConstantInt::get(i64_type, 0),
                            comparison->name);
}

llvm::Value *CodeGen::emit_numeric_op(Token::Kind op, llvm::Value *lhs,
                                      llvm::Value *rhs, const Node &parent) {
  if (!lhs->getType()->isDoubleTy() && !rhs->getType()->isDoubleTy())
    return emit_int_op(op, lhs, rhs, parent);
  if (lhs->getType()->isIntegerTy(64))
    lhs = builder.CreateSIToFP(lhs, f64_type, "itof");
  if (rhs->getType()->isIntegerTy(64))
    rhs = builder.CreateSIToFP(rhs, f64_type, "itof");
  return emit_float_op(op, lhs, rhs, parent);
}

llvm::Value *CodeGen::emit_float_op(Token::Kind op, llvm::Value *lhs,
                                    llvm::Value *rhs, const Node &parent) {
  if (auto comparison = comparison_of(op))
    return builder.CreateFCmp(comparison->float_predicate, lhs, rhs,
                              comparison->name);
  using K = Token::Kind;
  switch (op) {
  case K::Add:
    return builder.CreateFAdd(lhs, rhs, "fadd");
  case K::Sub:
    return builder.CreateFSub(lhs, rhs, "fsub");
  case K::Multiply:
    return builder.CreateFMul(lhs, rhs, "fmul");
  case K::Divide:
    return emit_float_division(lhs, rhs, parent);
  case K::Modulo:
    return builder.CreateFRem(lhs, rhs, "fmod");
  case K::Pow:
    return emit_float_pow(lhs, rhs);
  default:
    return nullptr;
  }
}

llvm::Value *CodeGen::emit_int_op(Token::Kind op, llvm::Value *lhs,
                                  llvm::Value *rhs, const Node &parent) {
  if (auto comparison = comparison_of(op))
    return builder.CreateICmp(comparison->int_predicate, lhs, rhs,
                              comparison->name);
  using K = Token::Kind;
  switch (op) {
  case K::Add:
    return builder.CreateAdd(lhs, rhs, "add");
  case K::Sub:
    return builder.CreateSub(lhs, rhs, "sub");
  case K::Multiply:
    return builder.CreateMul(lhs, rhs, "mul");
  case K::Divide:
  case K::Modulo:
    return emit_int_division(op, lhs, rhs, parent);
  case K::Pow:
    return emit_int_pow(lhs, rhs);
  case K::BitwiseAnd:
    return builder.CreateAnd(lhs, rhs, "band");
  case K::BitwiseOr:
    return builder.CreateOr(lhs, rhs, "bor");
  case K::BitwiseXor:
    return builder.CreateXor(lhs, rhs, "bxor");
  case K::LeftShift:
    return builder.CreateShl(lhs, rhs, "shl");
  case K::RightShift:
    return builder.CreateAShr(lhs, rhs, "shr");
  default:
    return nullptr;
  }
}

llvm::Value *CodeGen::emit_float_division(llvm::Value *lhs, llvm::Value *rhs,
                                          const Node &parent) {
  auto *quotient = builder.CreateFDiv(lhs, rhs, "fdiv");
  auto result = semantic_type(parent);
  if (result && result->kind == TypeKind::Union)
    return emit_union_wrap(quotient, analyzer.builtins.float_type, result);
  return quotient;
}

llvm::Value *CodeGen::emit_int_division(Token::Kind op, llvm::Value *lhs,
                                        llvm::Value *rhs, const Node &parent) {
  auto result = semantic_type(parent);
  if (result && result->kind == TypeKind::Union)
    return emit_checked_division(lhs, rhs, op, result);
  return op == Token::Kind::Modulo ? builder.CreateSRem(lhs, rhs, "mod")
                                   : builder.CreateSDiv(lhs, rhs, "div");
}

// `sdiv`/`srem` by zero does not produce a value — it faults, and the fault is
// what the `T | error` type exists to prevent. The divisor is tested first and
// the error path takes a shared rodata singleton, so a division that never
// divides by zero costs one predictable branch and no allocation.
llvm::Value *CodeGen::emit_checked_division(llvm::Value *lhs, llvm::Value *rhs,
                                            Token::Kind op,
                                            const TypePtr &result_union) {
  auto *func = builder.GetInsertBlock()->getParent();
  auto *zero = llvm::ConstantInt::get(rhs->getType(), 0);
  auto *by_zero = builder.CreateICmpEQ(rhs, zero, "div.by_zero");

  auto *err_bb = llvm::BasicBlock::Create(context, "div.err", func);
  auto *ok_bb = llvm::BasicBlock::Create(context, "div.ok", func);
  auto *merge_bb = llvm::BasicBlock::Create(context, "div.merge", func);
  builder.CreateCondBr(by_zero, err_bb, ok_bb);

  builder.SetInsertPoint(err_bb);
  llvm_type(analyzer.builtins.divide_by_zero_type);
  auto &err_info =
      std::get<StructTypeInfo>(analyzer.builtins.divide_by_zero_type->detail);
  auto *box = emit_error_singleton(err_info, op == Token::Kind::Modulo
                                                ? "remainder by zero"
                                                : "division by zero");
  auto *err_val =
      emit_union_wrap(box, analyzer.builtins.error_base, result_union);
  auto *err_end_bb = builder.GetInsertBlock();
  builder.CreateBr(merge_bb);

  builder.SetInsertPoint(ok_bb);
  llvm::Value *quotient = op == Token::Kind::Modulo
                              ? builder.CreateSRem(lhs, rhs, "mod")
                              : builder.CreateSDiv(lhs, rhs, "div");
  auto *ok_val = emit_union_wrap(quotient, analyzer.builtins.int_type,
                                 result_union);
  auto *ok_end_bb = builder.GetInsertBlock();
  builder.CreateBr(merge_bb);

  builder.SetInsertPoint(merge_bb);
  auto *phi = builder.CreatePHI(ok_val->getType(), 2, "div.result");
  phi->addIncoming(err_val, err_end_bb);
  phi->addIncoming(ok_val, ok_end_bb);
  return phi;
}

llvm::Value *CodeGen::finish_operator_result(const BinaryExprNode &node,
                                             const std::string &method,
                                             llvm::Value *result) {
  using K = Token::Kind;
  if (!result)
    return nullptr;

  if (method == "Compare") {
    // Comparison: Less=0, Equal=1, Greater=2.
    auto *less = llvm::ConstantInt::get(i64_type, 0);
    auto *equal = llvm::ConstantInt::get(i64_type, 1);
    auto *greater = llvm::ConstantInt::get(i64_type, 2);
    switch (node.op) {
    case K::LessThan:
      return builder.CreateICmpEQ(result, less, "lt");
    case K::LessThanEqual:
      return builder.CreateICmpNE(result, greater, "le");
    case K::GreaterThan:
      return builder.CreateICmpEQ(result, greater, "gt");
    case K::GreaterThanEqual:
      return builder.CreateICmpNE(result, less, "ge");
    case K::Equal:
      return builder.CreateICmpEQ(result, equal, "eq");
    case K::NotEqual:
      return builder.CreateICmpNE(result, equal, "ne");
    default:
      return result;
    }
  }

  if (method == "Equals" && node.op == K::NotEqual)
    return builder.CreateNot(result, "ne");

  return result;
}

// An overloaded operator is an ordinary receiver call, and is emitted as one.
// Building the call here by hand is what broke it: the sret/byval ABI lives on
// the declaration, so a call site that re-derives the return type from the
// method name cannot agree with the definition it is calling.
llvm::Value *CodeGen::emit_struct_binary_op(const BinaryExprNode &node,
                                             const TypePtr &lhs_sem,
                                             const std::string &method) {
  auto &info = std::get<StructTypeInfo>(lhs_sem->detail);
  const auto *method_fi = operator_method_signature(info, method);
  if (!method_fi)
    return nullptr;

  auto *callee =
      forward_declare_method(struct_method_link_name(info, method), *method_fi);
  auto *self = emit_borrowed_operand(*node.lhs);
  auto *rhs_val = emit_argument(
      *node.rhs, method_fi->params.empty() ? nullptr : method_fi->params[0],
      true);
  if (!callee || !self || !rhs_val)
    return nullptr;

  return finish_operator_result(
      node, method,
      emit_call(callee, self, {rhs_val}));
}

llvm::Value *CodeGen::emit_float_pow(llvm::Value *base, llvm::Value *exp) {
  auto *intrin = llvm::Intrinsic::getDeclaration(
      module.get(), llvm::Intrinsic::pow, {f64_type});
  return builder.CreateCall(intrin, {base, exp}, "pow");
}

llvm::Value *CodeGen::emit_int_pow(llvm::Value *base, llvm::Value *exp) {
  auto *func = builder.GetInsertBlock()->getParent();
  auto *result = create_entry_alloca(func, "pow.result", i64_type);
  auto *idx = create_entry_alloca(func, "pow.i", i64_type);
  builder.CreateStore(llvm::ConstantInt::get(i64_type, 1), result);
  builder.CreateStore(llvm::ConstantInt::get(i64_type, 0), idx);

  auto *cond_bb = llvm::BasicBlock::Create(context, "pow.cond", func);
  auto *body_bb = llvm::BasicBlock::Create(context, "pow.body", func);
  auto *end_bb = llvm::BasicBlock::Create(context, "pow.end", func);

  builder.CreateBr(cond_bb);
  builder.SetInsertPoint(cond_bb);
  auto *i_val = builder.CreateLoad(i64_type, idx, "pow.i.val");
  auto *done = builder.CreateICmpSGE(i_val, exp, "pow.done");
  builder.CreateCondBr(done, end_bb, body_bb);

  builder.SetInsertPoint(body_bb);
  auto *r_val = builder.CreateLoad(i64_type, result, "pow.r.val");
  builder.CreateStore(builder.CreateMul(r_val, base, "pow.next.r"), result);
  builder.CreateStore(
      builder.CreateAdd(i_val, llvm::ConstantInt::get(i64_type, 1),
                        "pow.next.i"),
      idx);
  builder.CreateBr(cond_bb);

  builder.SetInsertPoint(end_bb);
  return builder.CreateLoad(i64_type, result, "pow.val");
}

} // namespace saga

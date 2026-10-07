// Copyright 2026 Rob Thornton
// SPDX-License-Identifier: MIT

// Error promotion: `?` tests the union it sits on and, when it holds an error,
// abandons the rest of the expression by jumping to the landing its enclosing
// root expression set up. The value path continues on the purified type.

#include "ir/codegen.hpp"
#include "util/internal_error.hpp"

#include <llvm/IR/Constants.h>

namespace saga {

namespace {

llvm::Value *tag_is_error(llvm::IRBuilder<> &builder, llvm::LLVMContext &context,
                          llvm::Value *tag, const TypePtr &union_sem) {
  auto &info = std::get<UnionTypeInfo>(union_sem->detail);
  auto *i8_ty = llvm::Type::getInt8Ty(context);
  llvm::Value *result = llvm::ConstantInt::get(llvm::Type::getInt1Ty(context), 0);
  for (size_t i = 0; i < info.alternatives.size(); ++i) {
    if (!is_error_valued(info.alternatives[i]))
      continue;
    auto *cmp = builder.CreateICmpEQ(
        tag, llvm::ConstantInt::get(i8_ty, static_cast<int>(i)), "promote.cmp");
    result = builder.CreateOr(result, cmp, "promote.is_err");
  }
  return result;
}

} // namespace

TypePtr CodeGen::root_expr_type(const Node &node) const {
  auto it = analyzer.promotion_root_types.find(&node);
  if (it != analyzer.promotion_root_types.end())
    return unwrap_alias(it->second);
  return semantic_type(node);
}

llvm::Value *CodeGen::emit_error_escape(llvm::Value *operand,
                                        const TypePtr &operand_sem) {
  if (!operand || !operand_sem || operand_sem->kind != TypeKind::Union ||
      !is_impure_union(operand_sem) || promote_landings_.empty())
    return operand;

  auto &landing = promote_landings_.back();
  auto *func = builder.GetInsertBlock()->getParent();
  auto *union_st = get_union_llvm_type(operand_sem);

  llvm::Value *union_ptr = operand;
  if (!union_ptr->getType()->isPointerTy() ||
      llvm::isa<llvm::LoadInst>(union_ptr)) {
    auto *tmp = create_entry_alloca(func, "promote.union", union_st);
    builder.CreateStore(operand, tmp);
    union_ptr = tmp;
  }

  auto *tag_gep = builder.CreateStructGEP(union_st, union_ptr, 0, "promote.tag");
  auto *tag = builder.CreateLoad(llvm::Type::getInt8Ty(context), tag_gep,
                                 "promote.tag.val");

  auto *err_bb = llvm::BasicBlock::Create(context, "promote.raise", func);
  auto *ok_bb = llvm::BasicBlock::Create(context, "promote.ok", func);
  builder.CreateCondBr(tag_is_error(builder, context, tag, operand_sem), err_bb,
                       ok_bb);

  // The error travels in the root's union, whose alternatives differ from this
  // operand's, so it is converted rather than copied.
  builder.SetInsertPoint(err_bb);
  auto *raised = emit_union_convert(union_ptr, operand_sem, landing.result_type);
  auto *landing_st = get_union_llvm_type(landing.result_type);
  if (raised)
    builder.CreateStore(builder.CreateLoad(landing_st, raised, "promote.raised"),
                        landing.slot);
  release_temporaries(landing.temporaries_depth);
  builder.CreateBr(landing.err_bb);

  builder.SetInsertPoint(ok_bb);
  return emit_union_purified(union_ptr, tag, operand_sem);
}

llvm::Value *CodeGen::emit_operand(const Node &node) {
  auto *val = emit_expr(node);
  if (!analyzer.bubbled_operands.count(&node))
    return val;
  return emit_error_escape(val, semantic_type(node));
}

TypePtr CodeGen::operand_type(const Node &node) const {
  auto it = analyzer.bubbled_operands.find(&node);
  if (it != analyzer.bubbled_operands.end())
    return unwrap_alias(it->second);
  return semantic_type(node);
}

llvm::Value *CodeGen::emit_promote_expr(const PromoteExprNode &node) {
  if (promote_landings_.empty())
    internal_error("'?' reached codegen with no landing, which the analyzer "
                   "should have rejected");
  return emit_error_escape(emit_expr(*node.operand),
                           semantic_type(*node.operand));
}

llvm::Value *CodeGen::emit_root_expr(const Node &node) {
  auto it = analyzer.promotion_root_types.find(&node);
  if (it == analyzer.promotion_root_types.end())
    return emit_expr(node);

  auto root_type = unwrap_alias(it->second);
  auto *root_st = get_union_llvm_type(root_type);
  if (!root_st)
    return emit_expr(node);

  auto *func = builder.GetInsertBlock()->getParent();
  auto *slot = create_entry_alloca(func, "promote.result", root_st);
  auto *err_bb = llvm::BasicBlock::Create(context, "promote.landing");
  auto *done_bb = llvm::BasicBlock::Create(context, "promote.done");

  promote_landings_.push_back({err_bb, slot, root_type, temporaries_.size()});
  auto *val = emit_expr(node);
  auto value_sem = semantic_type(node);
  promote_landings_.pop_back();

  if (val && value_sem) {
    if (auto *wrapped = as_union_ptr(val, value_sem, root_type))
      builder.CreateStore(builder.CreateLoad(root_st, wrapped, "promote.ok.val"),
                          slot);
  }
  builder.CreateBr(done_bb);

  func->insert(func->end(), err_bb);
  builder.SetInsertPoint(err_bb);
  builder.CreateBr(done_bb);

  func->insert(func->end(), done_bb);
  builder.SetInsertPoint(done_bb);
  return builder.CreateLoad(root_st, slot, "promote.result.val");
}

} // namespace saga

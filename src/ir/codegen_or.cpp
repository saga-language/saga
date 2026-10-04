// Copyright 2026 Rob Thornton
// SPDX-License-Identifier: MIT

// The `or` expression: strips the error alternatives off a union and runs a
// handler on the error path. The union arithmetic is what makes this long —
// what survives stripping decides whether the result is still a union.

#include "ir/codegen.hpp"

#include <llvm/IR/Constants.h>

namespace saga {

// Extracting the non-error value is not a load when more than one value
// alternative survives: the tag indexes the *original* union, so each surviving
// alternative has to be re-tagged into the purified one before the payload is
// copied across.
llvm::Value *CodeGen::emit_union_purified(llvm::Value *union_ptr,
                                          llvm::Value *tag,
                                          const TypePtr &union_sem) {
  TypePtr purified = strip_error_from_union(union_sem);
  if (!purified)
    return nullptr;

  auto *union_st = get_union_llvm_type(union_sem);
  if (purified->kind != TypeKind::Union)
    return emit_union_extract(union_ptr, purified, union_sem);

  auto &info = std::get<UnionTypeInfo>(union_sem->detail);
  auto &pur_info = std::get<UnionTypeInfo>(purified->detail);
  auto *func = builder.GetInsertBlock()->getParent();
  auto *i8_ty = llvm::Type::getInt8Ty(context);
  auto *purified_st = get_union_llvm_type(purified);
  auto *purified_alloca = create_entry_alloca(func, "pur.union", purified_st);
  builder.CreateStore(llvm::Constant::getNullValue(purified_st),
                      purified_alloca);

  std::vector<int> value_tags;
  for (size_t i = 0; i < info.alternatives.size(); ++i) {
    if (!is_error_valued(info.alternatives[i]))
      value_tags.push_back(static_cast<int>(i));
  }

  auto *remap_default = llvm::BasicBlock::Create(context, "pur.remap.def");
  auto *remap_merge = llvm::BasicBlock::Create(context, "pur.remap.merge");
  auto *sw = builder.CreateSwitch(tag, remap_default, value_tags.size());

  for (int orig_tag : value_tags) {
    auto *case_bb = llvm::BasicBlock::Create(
        context, "pur.remap." + std::to_string(orig_tag), func);
    sw->addCase(llvm::ConstantInt::get(i8_ty, orig_tag), case_bb);
    builder.SetInsertPoint(case_bb);

    int new_tag = 0;
    for (size_t pi = 0; pi < pur_info.alternatives.size(); ++pi) {
      if (types_equal(pur_info.alternatives[pi], info.alternatives[orig_tag])) {
        new_tag = static_cast<int>(pi);
        break;
      }
    }

    auto *ptag_gep =
        builder.CreateStructGEP(purified_st, purified_alloca, 0, "pur.tag");
    builder.CreateStore(llvm::ConstantInt::get(i8_ty, new_tag), ptag_gep);

    auto *src_payload =
        builder.CreateStructGEP(union_st, union_ptr, 1, "src.payload");
    auto *dst_payload =
        builder.CreateStructGEP(purified_st, purified_alloca, 1, "dst.payload");
    builder.CreateMemCpy(dst_payload, llvm::Align(1), src_payload,
                         llvm::Align(1), union_payload_size(purified));
    builder.CreateBr(remap_merge);
  }

  func->insert(func->end(), remap_default);
  builder.SetInsertPoint(remap_default);
  builder.CreateBr(remap_merge);

  func->insert(func->end(), remap_merge);
  builder.SetInsertPoint(remap_merge);
  return builder.CreateLoad(purified_st, purified_alloca, "pur.val");
}

// An `or` on a value that cannot hold an error has no handler to run.
bool CodeGen::or_has_handler(const OrExprNode &node) const {
  auto sem = root_expr_type(*node.expr);
  return sem && sem->kind == TypeKind::Union && is_impure_union(sem);
}

TypePtr CodeGen::or_result_type(const OrExprNode &node) const {
  return strip_error_from_union(root_expr_type(*node.expr));
}

llvm::Value *CodeGen::emit_or_expr(const OrExprNode &node) {
  auto *expr_val = emit_root_expr(*node.expr);
  if (!expr_val || !or_has_handler(node))
    return expr_val;

  auto expr_sem = root_expr_type(*node.expr);
  auto *union_ptr = or_union_address(expr_val, expr_sem);
  auto *tag_gep = builder.CreateStructGEP(get_union_llvm_type(expr_sem),
                                          union_ptr, 0, "or.tag");
  auto *tag = builder.CreateLoad(llvm::Type::getInt8Ty(context), tag_gep,
                                 "or.tag.val");
  auto *func = builder.GetInsertBlock()->getParent();
  auto *ok_bb = llvm::BasicBlock::Create(context, "or.ok", func);
  auto *err_bb = llvm::BasicBlock::Create(context, "or.err");
  builder.CreateCondBr(is_error_tag(tag, expr_sem), err_bb, ok_bb);

  auto result = or_result_type(node);
  auto join = open_join("or.merge", result, or_ownership(node));
  builder.SetInsertPoint(ok_bb);
  close_branch(join, emit_union_purified(union_ptr, tag, expr_sem), result,
               value_ownership(*node.expr));
  start_block(err_bb);
  emit_or_handler(node, union_ptr, expr_sem, join);
  return finish_join(join, "or.result");
}

llvm::Value *CodeGen::or_union_address(llvm::Value *val,
                                       const TypePtr &union_sem) {
  if (val->getType()->isPointerTy() && !llvm::isa<llvm::LoadInst>(val))
    return val;
  auto *tmp = create_entry_alloca(builder.GetInsertBlock()->getParent(),
                                  "or.union", get_union_llvm_type(union_sem));
  builder.CreateStore(val, tmp);
  return tmp;
}

llvm::Value *CodeGen::is_error_tag(llvm::Value *tag,
                                   const TypePtr &union_sem) {
  auto *i8_ty = llvm::Type::getInt8Ty(context);
  auto &info = std::get<UnionTypeInfo>(union_sem->detail);
  llvm::Value *is_err = nullptr;
  for (size_t i = 0; i < info.alternatives.size(); ++i) {
    if (!is_error_valued(info.alternatives[i]))
      continue;
    auto *cmp = builder.CreateICmpEQ(tag, llvm::ConstantInt::get(i8_ty, i),
                                     "or.is_err");
    is_err = is_err ? builder.CreateOr(is_err, cmp, "or.any_err") : cmp;
  }
  return is_err;
}

// The pipe names the error for the handler's duration; the payload's first
// word is the error's interface pointer, whichever path produced it.
void CodeGen::emit_or_handler(const OrExprNode &node, llvm::Value *union_ptr,
                              const TypePtr &union_sem, BranchJoin &join) {
  llvm::AllocaInst *displaced = nullptr;
  std::string pipe_name = node.pipe ? std::string(node.pipe->name) : "";
  if (node.pipe) {
    auto *ptr_type = llvm::PointerType::getUnqual(context);
    auto *slot = create_entry_alloca(builder.GetInsertBlock()->getParent(),
                                     pipe_name, ptr_type);
    auto *payload = builder.CreateStructGEP(get_union_llvm_type(union_sem),
                                            union_ptr, 1, "err.payload.gep");
    builder.CreateStore(
        builder.CreateLoad(ptr_type, payload, "err.payload.val"), slot);
    auto it = locals.find(pipe_name);
    displaced = it == locals.end() ? nullptr : it->second;
    locals[pipe_name] = slot;
  }

  auto &fallback = std::get<BlockNode>(node.fallback->data);
  auto *val = emit_block(fallback);
  close_branch(join, val, block_result_type(fallback),
               body_ownership(node.fallback.get(), join.result));

  if (displaced)
    locals[pipe_name] = displaced;
  else if (node.pipe)
    locals.erase(pipe_name);
}

} // namespace saga

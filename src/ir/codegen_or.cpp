// Copyright 2026 Rob Thornton
// SPDX-License-Identifier: MIT

// The `or` expression: strips the error alternatives off a union and runs a
// handler on the error path. The union arithmetic is what makes this long —
// what survives stripping decides whether the result is still a union.

#include "ir/codegen.hpp"
#include "util/internal_error.hpp"

#include <llvm/IR/Constants.h>

#include <cstdint>

namespace saga {

// ===========================================================================
// Or expression (error stripping)
// ===========================================================================

// Stripping the error alternatives can leave more than one behind, and then the
// result is still a union. The handler's value has to reach the merge as that
// same union, so it is wrapped (or remapped, if it is a union already) here.
llvm::Value *CodeGen::fallback_as_union(llvm::Value *val,
                                        const BlockNode &block,
                                        const TypePtr &target) {
  auto sem = block_result_type(block);
  auto *target_st = get_union_llvm_type(target);
  if (!sem || !target_st)
    return val;

  llvm::Value *slot = nullptr;
  if (sem->kind == TypeKind::Union) {
    if (!val->getType()->isPointerTy()) {
      auto *func = builder.GetInsertBlock()->getParent();
      auto *spill = create_entry_alloca(func, "or.fb.spill", val->getType());
      builder.CreateStore(val, spill);
      val = spill;
    }
    slot = emit_union_convert(val, sem, target);
  } else {
    slot = emit_union_wrap(val, sem, target);
  }
  return slot ? builder.CreateLoad(target_st, slot, "or.fb.union") : val;
}

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

llvm::Value *CodeGen::emit_or_expr(const OrExprNode &node) {
  // Emit the expression that may produce a union with Error.
  auto *expr_val = emit_root_expr(*node.expr);
  if (!expr_val)
    return nullptr;

  auto expr_sem = root_expr_type(*node.expr);
  if (!expr_sem)
    return expr_val;

  // If the expression is not a union, just return the value.
  if (expr_sem->kind != TypeKind::Union)
    return expr_val;

  // Check if this is an impure union (contains Error).
  if (!is_impure_union(expr_sem))
    return expr_val;

  auto *union_st = get_union_llvm_type(expr_sem);
  if (!union_st)
    return expr_val;

  auto *func = builder.GetInsertBlock()->getParent();

  // The expr_val should be an alloca (pointer to the union struct).
  // If it's not already a pointer to the union, we need to handle that.
  llvm::Value *union_ptr = expr_val;

  // If union_ptr is a loaded value (struct type, not pointer), store it.
  if (!union_ptr->getType()->isPointerTy() ||
      (llvm::isa<llvm::LoadInst>(union_ptr))) {
    auto *tmp = create_entry_alloca(func, "or.union", union_st);
    builder.CreateStore(expr_val, tmp);
    union_ptr = tmp;
  }

  // Load the tag.
  auto *tag_gep = builder.CreateStructGEP(union_st, union_ptr, 0, "or.tag");
  auto *tag = builder.CreateLoad(llvm::Type::getInt8Ty(context), tag_gep,
                                  "or.tag.val");

  // Find which tag values correspond to Error types.
  auto &info = std::get<UnionTypeInfo>(expr_sem->detail);
  std::vector<int> error_tags;
  for (size_t i = 0; i < info.alternatives.size(); ++i) {
    if (is_error_valued(info.alternatives[i]))
      error_tags.push_back(static_cast<int>(i));
  }

  // Create basic blocks.
  auto *ok_bb = llvm::BasicBlock::Create(context, "or.ok", func);
  auto *err_bb = llvm::BasicBlock::Create(context, "or.err");
  auto *merge_bb = llvm::BasicBlock::Create(context, "or.merge");

  // Branch based on whether the tag is an error tag.
  // If there's only one error tag, simple comparison.
  // For multiple error tags, we'd need an or-chain, but typically there's
  // just one Error interface in the union.
  if (error_tags.size() == 1) {
    auto *is_err = builder.CreateICmpEQ(
        tag,
        llvm::ConstantInt::get(llvm::Type::getInt8Ty(context), error_tags[0]),
        "or.is_err");
    builder.CreateCondBr(is_err, err_bb, ok_bb);
  } else {
    // Multiple error tags — build an OR chain.
    llvm::Value *is_err = llvm::ConstantInt::get(
        llvm::Type::getInt1Ty(context), 0);
    for (int et : error_tags) {
      auto *cmp = builder.CreateICmpEQ(
          tag,
          llvm::ConstantInt::get(llvm::Type::getInt8Ty(context), et),
          "or.cmp");
      is_err = builder.CreateOr(is_err, cmp, "or.any_err");
    }
    builder.CreateCondBr(is_err, err_bb, ok_bb);
  }

  // ── OK block: extract the non-error value ──────────────────────────
  builder.SetInsertPoint(ok_bb);

  TypePtr purified = strip_error_from_union(expr_sem);
  llvm::Value *ok_val = emit_union_purified(union_ptr, tag, expr_sem);

  if (!ok_val)
    ok_val = llvm::Constant::getNullValue(
        purified ? llvm_type(purified) : i64_type);

  builder.CreateBr(merge_bb);
  auto *ok_end_bb = builder.GetInsertBlock();

  // ── Error block: emit fallback ─────────────────────────────────────
  func->insert(func->end(), err_bb);
  builder.SetInsertPoint(err_bb);

  // Bind the pipe variable to the Error payload extracted from the
  // union.  The payload first 8 bytes hold the interface fat pointer
  // produced by whichever path produced the Error (e.g. Task.Wait's
  // saga_error_from_trap).
  if (node.pipe) {
    std::string pipe_name(node.pipe->name);
    auto *ptr_type = llvm::PointerType::getUnqual(context);
    auto *err_alloca = create_entry_alloca(func, pipe_name, ptr_type);
    auto *payload_gep = builder.CreateStructGEP(union_st, union_ptr, 1,
                                                 "err.payload.gep");
    auto *err_val = builder.CreateLoad(ptr_type, payload_gep,
                                        "err.payload.val");
    builder.CreateStore(err_val, err_alloca);
    locals[pipe_name] = err_alloca;
  }

  auto &fallback_block = std::get<BlockNode>(node.fallback->data);
  auto *fallback_val = emit_block(fallback_block);

  // The fallback value must match the purified type.
  if (!fallback_val && ok_val)
    fallback_val = llvm::Constant::getNullValue(ok_val->getType());

  if (fallback_val && ok_val && fallback_val->getType() != ok_val->getType()) {
    if (purified && purified->kind == TypeKind::Union)
      fallback_val = fallback_as_union(fallback_val, fallback_block, purified);
    // A struct payload leaves the union as a value and leaves the handler as
    // an address, so the two branches settle on the address. This runs after
    // the union conversion above, never before: once both sides are pointers
    // their LLVM types are equal and a missing conversion looks like a match.
    if (fallback_val->getType() != ok_val->getType()) {
      auto saved = builder.saveIP();
      builder.SetInsertPoint(ok_end_bb->getTerminator());
      ok_val = spill_aggregate(ok_val, "or.ok.spill");
      builder.restoreIP(saved);
      fallback_val = spill_aggregate(fallback_val, "or.fallback.spill");
    }
    if (fallback_val->getType() != ok_val->getType())
      internal_error("`or` handler produced a value the result type cannot "
                     "hold, which the analyzer should have rejected");
  }

  bool err_terminated = builder.GetInsertBlock()->getTerminator() != nullptr;
  if (!err_terminated)
    builder.CreateBr(merge_bb);
  auto *err_end_bb = builder.GetInsertBlock();

  // Clean up pipe variable.
  if (node.pipe) {
    locals.erase(std::string(node.pipe->name));
  }

  // ── Merge block ────────────────────────────────────────────────────
  func->insert(func->end(), merge_bb);
  builder.SetInsertPoint(merge_bb);

  if (ok_val && fallback_val &&
      ok_val->getType() == fallback_val->getType() && !err_terminated) {
    auto *phi = builder.CreatePHI(ok_val->getType(), 2, "or.result");
    phi->addIncoming(ok_val, ok_end_bb);
    phi->addIncoming(fallback_val, err_end_bb);
    return phi;
  }

  return ok_val;
}

} // namespace saga

// Copyright 2026 Rob Thornton
// SPDX-License-Identifier: MIT

// Index expressions: `a[i]` on arrays, maps and strings. A map lookup can
// fail, so its result is wrapped into the error union the analyzer gave it.

#include "ir/codegen.hpp"

#include <llvm/IR/Constants.h>

#include <cstdint>

namespace saga {

// ===========================================================================
// Index expressions
// ===========================================================================

llvm::Value *
CodeGen::wrap_indexed_lookup_in_error_union(llvm::Value *elem_ptr,
                                            llvm::Type *elem_ll,
                                            const TypePtr &val_type,
                                            const std::string &miss_msg) {
  auto result_union =
      make_union_type({val_type, analyzer.builtins.error_base});
  auto *union_st = get_union_llvm_type(result_union);
  if (!union_st)
    return nullptr;

  auto *func = builder.GetInsertBlock()->getParent();
  auto *ptr_ty = llvm::PointerType::getUnqual(context);
  auto *is_null = builder.CreateICmpEQ(
      elem_ptr, llvm::ConstantPointerNull::get(ptr_ty), "idx.is_null");

  auto *null_bb = llvm::BasicBlock::Create(context, "idx.null", func);
  auto *ok_bb = llvm::BasicBlock::Create(context, "idx.ok", func);
  auto *merge_bb = llvm::BasicBlock::Create(context, "idx.merge", func);

  builder.CreateCondBr(is_null, null_bb, ok_bb);

  builder.SetInsertPoint(null_bb);
  // The error payload is a Missing error box carrying the message, so
  // `or |err| { err.message }` reads it as a plain field. Field-less and
  // constant-message, it lowers to a shared rodata singleton (0 allocs).
  llvm_type(analyzer.builtins.missing_type);
  auto &missing_info =
      std::get<StructTypeInfo>(analyzer.builtins.missing_type->detail);
  auto *err_box = emit_error_singleton(missing_info, miss_msg);
  auto *err_wrapped =
      emit_union_wrap(err_box, analyzer.builtins.error_base, result_union);
  auto *null_end_bb = builder.GetInsertBlock();
  builder.CreateBr(merge_bb);

  builder.SetInsertPoint(ok_bb);
  auto *loaded = builder.CreateLoad(elem_ll, elem_ptr, "elem");
  auto *ok_wrapped = emit_union_wrap(loaded, val_type, result_union);
  auto *ok_end_bb = builder.GetInsertBlock();
  builder.CreateBr(merge_bb);

  builder.SetInsertPoint(merge_bb);
  auto *phi = builder.CreatePHI(ptr_ty, 2, "idx.union");
  phi->addIncoming(err_wrapped, null_end_bb);
  phi->addIncoming(ok_wrapped, ok_end_bb);
  return phi;
}

llvm::Value *CodeGen::emit_index_expr(const IndexExprNode &node) {
  auto *obj = emit_expr(*node.object);
  if (!obj)
    return nullptr;

  auto obj_sem = semantic_type(*node.object);

  // Slice form: `obj[a..b]`, `obj[..b]`, `obj[a..]`, `obj[..]`.  String
  // slicing is byte-indexed via saga_string_slice; absent endpoints flow
  // through as the INT64_MIN sentinel so the runtime clamps to [0, len].
  if (auto *slice = std::get_if<SliceNode>(&node.index->data)) {
    if (obj_sem && obj_sem->kind == TypeKind::String) {
      auto *sentinel = llvm::ConstantInt::get(
          i64_type, static_cast<uint64_t>(INT64_MIN), /*isSigned=*/true);
      llvm::Value *lo = sentinel;
      llvm::Value *hi = sentinel;
      if (slice->low) {
        lo = emit_expr(**slice->low);
        if (lo && lo->getType() != i64_type)
          lo = builder.CreateIntCast(lo, i64_type, true, "slice.lo");
      }
      if (slice->high) {
        hi = emit_expr(**slice->high);
        if (hi && hi->getType() != i64_type)
          hi = builder.CreateIntCast(hi, i64_type, true, "slice.hi");
      }
      if (!lo || !hi) return nullptr;
      return builder.CreateCall(module->getFunction("saga_string_slice"),
                                {obj, lo, hi}, "str.slice");
    }
    return nullptr;
  }

  // Scalar index form: `obj[i]`.
  if (obj_sem && obj_sem->kind == TypeKind::String) {
    auto *idx = emit_expr(*node.index);
    if (!idx) return nullptr;
    if (idx->getType() != i64_type)
      idx = builder.CreateIntCast(idx, i64_type, true, "str.idx");
    return builder.CreateCall(module->getFunction("saga_string_at"),
                              {obj, idx}, "str.at");
  }

  if (obj_sem && obj_sem->kind == TypeKind::Array) {
    auto *idx = emit_expr(*node.index);
    if (!idx)
      return nullptr;

    auto *at_fn = module->getFunction("saga_array_at");
    auto *elem_ptr = builder.CreateCall(at_fn, {obj, idx}, "at");

    auto &arr_info = std::get<ArrayTypeInfo>(obj_sem->detail);
    auto *elem_ll = llvm_type(arr_info.element);

    return wrap_indexed_lookup_in_error_union(
        elem_ptr, elem_ll, arr_info.element, "index out of bounds");
  }

  if (obj_sem && obj_sem->kind == TypeKind::Map) {
    auto *idx = emit_expr(*node.index);
    if (!idx)
      return nullptr;

    auto &map_info = std::get<MapTypeInfo>(obj_sem->detail);

    auto *func = builder.GetInsertBlock()->getParent();

    auto *key_tmp = create_entry_alloca(func, "map.idx.key", idx->getType());
    builder.CreateStore(idx, key_tmp);

    auto *get_fn = module->getFunction("saga_map_get");
    auto *val_ptr = builder.CreateCall(get_fn, {obj, key_tmp}, "map.get");

    auto *val_ll = llvm_type(map_info.value);
    return wrap_indexed_lookup_in_error_union(
        val_ptr, val_ll, map_info.value, "key not found");
  }

  // String indexing — deferred for now.
  return nullptr;
}

} // namespace saga

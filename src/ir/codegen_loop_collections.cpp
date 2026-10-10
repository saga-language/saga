// Copyright 2026 Rob Thornton
// SPDX-License-Identifier: MIT

// `for v : xs` over an array or a map, walked by index.

#include "ir/codegen.hpp"

#include <llvm/IR/Constants.h>

namespace saga {

void CodeGen::emit_for_range_array(const ForExprNode &node,
                                   const ForRangeClauseNode &range,
                                   llvm::Value *iterable,
                                   const TypePtr &iter_sem,
                                   const ForLoopBlocks &bbs) {
  auto *func = builder.GetInsertBlock()->getParent();
  auto &arr_info = std::get<ArrayTypeInfo>(iter_sem->detail);
  auto *elem_ll = llvm_type(arr_info.element);
  bool struct_elem = arr_info.element &&
                     arr_info.element->kind == TypeKind::Struct &&
                     elem_ll && elem_ll->isStructTy();

  auto *size_fn = module->getFunction("saga_array_size");
  auto *arr_len = builder.CreateCall(size_fn, {iterable}, "arr.len");

  auto *idx_alloca = create_entry_alloca(func, ".idx", i64_type);
  builder.CreateStore(llvm::ConstantInt::get(i64_type, 0), idx_alloca);

  llvm::AllocaInst *key_alloca = nullptr;
  llvm::AllocaInst *val_alloca = nullptr;
  if (range.vars.size() == 1) {
    val_alloca =
        create_entry_alloca(func, std::string(range.vars[0].name), elem_ll);
    locals[std::string(range.vars[0].name)] = val_alloca;
  } else if (range.vars.size() == 2) {
    key_alloca = create_entry_alloca(
        func, std::string(range.vars[0].name), i64_type);
    locals[std::string(range.vars[0].name)] = key_alloca;
    val_alloca = create_entry_alloca(
        func, std::string(range.vars[1].name), elem_ll);
    locals[std::string(range.vars[1].name)] = val_alloca;
  }

  builder.CreateBr(bbs.cond_bb);
  builder.SetInsertPoint(bbs.cond_bb);
  auto *cur_idx = builder.CreateLoad(i64_type, idx_alloca, "idx");
  auto *cmp = builder.CreateICmpSLT(cur_idx, arr_len, "range.cmp");
  builder.CreateCondBr(cmp, bbs.body_bb, bbs.exit_bb);

  enter_loop_body(bbs);

  auto *at_fn = module->getFunction("saga_array_at");
  auto *body_idx = builder.CreateLoad(i64_type, idx_alloca, "idx");
  auto *elem_ptr = builder.CreateCall(at_fn, {iterable, body_idx}, "at");

  if (key_alloca)
    builder.CreateStore(body_idx, key_alloca);
  if (val_alloca) {
    if (struct_elem) {
      auto sz = size_of(elem_ll);
      auto al = align_of(elem_ll);
      builder.CreateMemCpy(val_alloca, al, elem_ptr, al, sz);
    } else {
      auto *elem_val = builder.CreateLoad(elem_ll, elem_ptr, "elem");
      builder.CreateStore(elem_val, val_alloca);
    }
  }

  finish_loop_body(node, bbs.update_bb);

  start_block(bbs.update_bb);
  auto *upd_idx = builder.CreateLoad(i64_type, idx_alloca, "idx");
  auto *next_idx = builder.CreateAdd(
      upd_idx, llvm::ConstantInt::get(i64_type, 1), "idx.next");
  builder.CreateStore(next_idx, idx_alloca);
  builder.CreateBr(bbs.cond_bb);
}

void CodeGen::emit_for_range_map(const ForExprNode &node,
                                 const ForRangeClauseNode &range,
                                 llvm::Value *iterable,
                                 const TypePtr &iter_sem,
                                 const ForLoopBlocks &bbs) {
  auto *func = builder.GetInsertBlock()->getParent();
  auto &map_info = std::get<MapTypeInfo>(iter_sem->detail);

  auto *size_fn = module->getFunction("saga_map_size");
  auto *map_len = builder.CreateCall(size_fn, {iterable}, "map.len");

  auto *idx_alloca = create_entry_alloca(func, ".map.idx", i64_type);
  builder.CreateStore(llvm::ConstantInt::get(i64_type, 0), idx_alloca);

  auto *key_ll = llvm_type(map_info.key);
  auto *val_ll = llvm_type(map_info.value);
  llvm::AllocaInst *key_alloca = nullptr;
  llvm::AllocaInst *val_alloca = nullptr;

  if (range.vars.size() == 1) {
    val_alloca =
        create_entry_alloca(func, std::string(range.vars[0].name), val_ll);
    locals[std::string(range.vars[0].name)] = val_alloca;
  } else if (range.vars.size() == 2) {
    key_alloca =
        create_entry_alloca(func, std::string(range.vars[0].name), key_ll);
    locals[std::string(range.vars[0].name)] = key_alloca;
    val_alloca =
        create_entry_alloca(func, std::string(range.vars[1].name), val_ll);
    locals[std::string(range.vars[1].name)] = val_alloca;
  }

  builder.CreateBr(bbs.cond_bb);
  builder.SetInsertPoint(bbs.cond_bb);
  auto *cur_idx = builder.CreateLoad(i64_type, idx_alloca, "map.idx");
  auto *cmp = builder.CreateICmpSLT(cur_idx, map_len, "map.cmp");
  builder.CreateCondBr(cmp, bbs.body_bb, bbs.exit_bb);

  enter_loop_body(bbs);

  auto *body_idx = builder.CreateLoad(i64_type, idx_alloca, "map.idx");

  auto load_or_memcpy = [&](llvm::AllocaInst *dst, llvm::Type *ll,
                            const char *fn_name, const char *vname) {
    auto *fn = module->getFunction(fn_name);
    auto *ptr = builder.CreateCall(fn, {iterable, body_idx},
                                   std::string(vname) + ".ptr");
    if (ll->isStructTy()) {
      auto sz = size_of(ll);
      auto al = align_of(ll);
      builder.CreateMemCpy(dst, al, ptr, al, sz);
    } else {
      auto *v = builder.CreateLoad(ll, ptr, vname);
      builder.CreateStore(v, dst);
    }
  };

  if (key_alloca)
    load_or_memcpy(key_alloca, key_ll, "saga_map_key_at", "map.key");
  if (val_alloca)
    load_or_memcpy(val_alloca, val_ll, "saga_map_value_at", "map.val");

  finish_loop_body(node, bbs.update_bb);

  start_block(bbs.update_bb);
  auto *upd_idx = builder.CreateLoad(i64_type, idx_alloca, "map.idx");
  auto *next_idx = builder.CreateAdd(
      upd_idx, llvm::ConstantInt::get(i64_type, 1), "map.idx.next");
  builder.CreateStore(next_idx, idx_alloca);
  builder.CreateBr(bbs.cond_bb);
}

} // namespace saga

// Copyright 2026 Rob Thornton
// SPDX-License-Identifier: MIT

// `for v : src` over a source that hands out one value at a time: a task's
// channel, or a struct with a `Next` method.

#include "ir/codegen.hpp"

#include <llvm/IR/Constants.h>

namespace saga {

// `for msg : task` reads from a companion channel pointer the spawn site
// stored in a local variable named `<task>.channel`. Without that
// companion, no message stream exists, so we fall through to exit.
void CodeGen::emit_for_range_task(const ForExprNode &node,
                                  const ForRangeClauseNode &range,
                                  const StructTypeInfo &st_info,
                                  const ForLoopBlocks &bbs) {
  auto *func = builder.GetInsertBlock()->getParent();

  std::string task_name;
  if (auto *iter_id = std::get_if<IdentifierNode>(&range.iterable->data))
    task_name = std::string(iter_id->name);

  llvm::Value *ch_ptr = nullptr;
  if (!task_name.empty()) {
    auto ch_it = locals.find(task_name + ".channel");
    if (ch_it != locals.end())
      ch_ptr = builder.CreateLoad(llvm::PointerType::getUnqual(context),
                                  ch_it->second, "ch.ptr");
  }
  if (!ch_ptr) {
    builder.CreateBr(bbs.exit_bb);
    return;
  }

  llvm::Type *msg_ll = i64_type;
  TypePtr elem_sem;
  if (!st_info.type_args.empty())
    elem_sem = st_info.type_args[0];
  if (elem_sem)
    msg_ll = storage_type(elem_sem);

  llvm::AllocaInst *msg_alloca = nullptr;
  if (!range.vars.empty()) {
    std::string vname(range.vars[0].name);
    msg_alloca = create_entry_alloca(func, vname, msg_ll);
    locals[vname] = msg_alloca;
  } else {
    msg_alloca = create_entry_alloca(func, ".msg.buf", msg_ll);
  }

  builder.CreateBr(bbs.cond_bb);
  builder.SetInsertPoint(bbs.cond_bb);
  auto *recv_fn = module->getFunction("saga_channel_recv");
  auto *rc =
      builder.CreateCall(recv_fn, {ch_ptr, msg_alloca}, "recv.rc");
  auto *eof = builder.CreateICmpEQ(
      rc,
      llvm::ConstantInt::get(llvm::Type::getInt32Ty(context), -1),
      "recv.eof");
  builder.CreateCondBr(eof, bbs.exit_bb, bbs.body_bb);

  enter_loop_body(bbs);
  finish_loop_body(node, bbs.update_bb);

  start_block(bbs.update_bb);
  builder.CreateBr(bbs.cond_bb);
}

// `for v : iter` calls iter.Next() each step, where Next() returns
// T | Error. The loop ends when the result tag matches the Error variant.
void CodeGen::emit_for_range_iterable_struct(const ForExprNode &node,
                                             const ForRangeClauseNode &range,
                                             llvm::Value *iterable,
                                             const TypePtr &iter_sem,
                                             const ForLoopBlocks &bbs) {
  auto *func = builder.GetInsertBlock()->getParent();
  auto &st_info = std::get<StructTypeInfo>(iter_sem->detail);

  auto elem_sem = iterable_next_elem_type_of(*range.iterable);
  if (!elem_sem) {
    builder.CreateBr(bbs.exit_bb);
    return;
  }
  auto *elem_ll = llvm_type(elem_sem);
  auto *ptr_type = llvm::PointerType::getUnqual(context);

  std::string next_link_name;
  auto ml_it = struct_method_links.find(st_info.name);
  if (ml_it != struct_method_links.end()) {
    for (auto &[lname, mname] : ml_it->second)
      if (mname == "Next") { next_link_name = lname; break; }
  }
  if (next_link_name.empty())
    next_link_name = mangle(st_info.name + "__Next");

  TypePtr next_ret_sem;
  for (auto &m : st_info.methods) {
    if (m.name == "Next" && m.signature &&
        m.signature->kind == TypeKind::Func) {
      auto &fi = std::get<FuncTypeInfo>(m.signature->detail);
      if (fi.return_type)
        next_ret_sem = fi.return_type;
      break;
    }
  }
  if (!next_ret_sem)
    next_ret_sem =
        make_union_type({elem_sem, analyzer.builtins.error_base});

  auto *union_st = get_union_llvm_type(next_ret_sem);

  int error_tag = 1;
  if (next_ret_sem->kind == TypeKind::Union) {
    auto &ui = std::get<UnionTypeInfo>(next_ret_sem->detail);
    for (size_t i = 0; i < ui.alternatives.size(); ++i)
      if (ui.alternatives[i]->kind == TypeKind::Interface)
        error_tag = static_cast<int>(i);
  }

  auto *next_fn = module->getFunction(next_link_name);
  if (!next_fn && union_st) {
    auto *ret_ll = static_cast<llvm::Type *>(union_st);
    auto *fn_type = llvm::FunctionType::get(ret_ll, {ptr_type}, false);
    next_fn = llvm::Function::Create(fn_type, llvm::Function::ExternalLinkage,
                                     next_link_name, module.get());
  }

  llvm::Value *self_ptr = nullptr;
  if (auto *id = std::get_if<IdentifierNode>(&range.iterable->data)) {
    auto local_it = locals.find(std::string(id->name));
    if (local_it != locals.end()) {
      auto *alloca = local_it->second;
      auto st_it2 = struct_types.find(st_info.name);
      if (st_it2 != struct_types.end() &&
          alloca->getAllocatedType() == st_it2->second)
        self_ptr = alloca;
    }
  }
  if (!self_ptr) {
    auto st_it2 = struct_types.find(st_info.name);
    if (st_it2 != struct_types.end() &&
        iterable->getType() == st_it2->second) {
      auto *tmp = create_entry_alloca(func, "iter.self", st_it2->second);
      builder.CreateStore(iterable, tmp);
      self_ptr = tmp;
    } else {
      self_ptr = iterable;
    }
  }

  llvm::AllocaInst *val_alloca = nullptr;
  if (!range.vars.empty()) {
    std::string vname(range.vars[0].name);
    val_alloca = create_entry_alloca(func, vname, elem_ll);
    locals[vname] = val_alloca;
  }

  llvm::AllocaInst *result_alloca = nullptr;
  if (union_st)
    result_alloca = create_entry_alloca(func, "next.result", union_st);

  if (!next_fn || !result_alloca || !self_ptr) {
    builder.CreateBr(bbs.exit_bb);
    return;
  }

  builder.CreateBr(bbs.cond_bb);
  builder.SetInsertPoint(bbs.cond_bb);
  auto *next_val = builder.CreateCall(next_fn, {self_ptr}, "next.val");
  builder.CreateStore(next_val, result_alloca);

  auto *tag_gep = builder.CreateStructGEP(union_st, result_alloca, 0,
                                          "next.tag.ptr");
  auto *tag = builder.CreateLoad(llvm::Type::getInt8Ty(context), tag_gep,
                                 "next.tag");
  auto *is_err = builder.CreateICmpEQ(
      tag,
      llvm::ConstantInt::get(llvm::Type::getInt8Ty(context), error_tag),
      "next.is_err");
  builder.CreateCondBr(is_err, bbs.exit_bb, bbs.body_bb);

  enter_loop_body(bbs);

  if (val_alloca) {
    auto *val = emit_union_extract(result_alloca, elem_sem, next_ret_sem);
    if (val)
      builder.CreateStore(val, val_alloca);
  }

  finish_loop_body(node, bbs.update_bb);

  start_block(bbs.update_bb);
  builder.CreateBr(bbs.cond_bb);
}

} // namespace saga

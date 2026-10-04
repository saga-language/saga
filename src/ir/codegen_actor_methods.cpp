// Copyright 2026 Rob Thornton
// SPDX-License-Identifier: MIT

// Methods on the actor handles: a Task names a spawned actor and a Context
// is the running actor's view of itself. Each lowers to a runtime call on
// the actor pointer the handle holds.

#include "ir/codegen.hpp"

#include <llvm/IR/Constants.h>

namespace saga {

std::optional<llvm::Value *>
CodeGen::emit_task_method_call(const std::string &method,
                               const StructTypeInfo &sinfo, llvm::Value *obj) {
  if (method == "Alive") {
    auto *fn = module->getFunction("saga_task_alive");
    auto *result = builder.CreateCall(fn, {obj}, "alive");
    return builder.CreateICmpNE(result, llvm::ConstantInt::get(i64_type, 0),
                                "alive.bool");
  }
  if (method == "Cancel") {
    builder.CreateCall(module->getFunction("saga_task_cancel"), {obj});
    return nullptr;
  }
  if (method == "Term") {
    builder.CreateCall(module->getFunction("saga_task_term"), {obj});
    return nullptr;
  }
  if (method == "Wait")
    return emit_task_wait(sinfo, obj);
  return std::nullopt;
}

// A completed actor's result is wrapped as the T of `T | error`; any other
// end becomes the Trapped error the runtime builds from it.
llvm::Value *CodeGen::emit_task_wait(const StructTypeInfo &sinfo,
                                     llvm::Value *obj) {
  auto *func = builder.GetInsertBlock()->getParent();
  auto *ptr_type = llvm::PointerType::getUnqual(context);

  TypePtr t_sem;
  if (!sinfo.type_args.empty())
    t_sem = sinfo.type_args[0];
  if (!t_sem)
    return nullptr;
  auto error_base_sem_pre = analyzer.builtins.error_base;
  auto union_sem = make_union_type({t_sem, error_base_sem_pre});
  auto *union_st = get_union_llvm_type(union_sem);

  auto *status_alloca = create_entry_alloca(func, "wait.status", i64_type);
  builder.CreateStore(llvm::ConstantInt::get(i64_type, 0), status_alloca);
  auto *result_ptr = builder.CreateCall(module->getFunction("saga_task_wait"),
                                        {obj, status_alloca}, "wait.result");
  auto *status = builder.CreateLoad(i64_type, status_alloca, "wait.status.val");
  auto *is_ok = builder.CreateICmpEQ(
      status,
      llvm::ConstantInt::get(i64_type, /*SAGA_RUNTIME_ACTOR_COMPLETED=*/2),
      "wait.ok");

  auto *bb_ok = llvm::BasicBlock::Create(context, "wait.ok.bb", func);
  auto *bb_err = llvm::BasicBlock::Create(context, "wait.err.bb");
  auto *bb_merge = llvm::BasicBlock::Create(context, "wait.merge");
  builder.CreateCondBr(is_ok, bb_ok, bb_err);

  builder.SetInsertPoint(bb_ok);
  llvm::Value *wrapped_ok = nullptr;
  if (t_sem) {
    auto *t_ll = llvm_type(t_sem);
    llvm::Value *t_val = nullptr;
    if (t_ll->isVoidTy()) {
      t_val = llvm::ConstantInt::get(llvm::Type::getInt8Ty(context), 0);
      wrapped_ok = emit_union_wrap(t_val, t_sem, union_sem);
    } else {
      t_val = builder.CreateLoad(t_ll, result_ptr, "wait.t.val");
      wrapped_ok = emit_union_wrap(t_val, t_sem, union_sem);
    }
  }
  if (!wrapped_ok)
    wrapped_ok = llvm::ConstantPointerNull::get(ptr_type);
  auto *bb_ok_end = builder.GetInsertBlock();
  builder.CreateBr(bb_merge);

  func->insert(func->end(), bb_err);
  builder.SetInsertPoint(bb_err);
  auto *err_box = builder.CreateCall(module->getFunction("saga_error_from_trap"),
                                     {obj}, "wait.err.box");
  auto error_base_sem = analyzer.builtins.error_base;
  auto *wrapped_err = emit_union_wrap(err_box, error_base_sem, union_sem);
  if (!wrapped_err)
    wrapped_err = llvm::ConstantPointerNull::get(ptr_type);
  auto *bb_err_end = builder.GetInsertBlock();
  builder.CreateBr(bb_merge);

  func->insert(func->end(), bb_merge);
  builder.SetInsertPoint(bb_merge);
  auto *phi = builder.CreatePHI(ptr_type, 2, "wait.union");
  phi->addIncoming(wrapped_ok, bb_ok_end);
  phi->addIncoming(wrapped_err, bb_err_end);
  (void)union_st;
  return phi;
}

std::optional<llvm::Value *>
CodeGen::emit_context_method_call(const CallExprNode &node,
                                  const std::string &method, llvm::Value *obj) {
  if (method == "Cancelled") {
    auto *fn = module->getFunction("saga_context_cancelled");
    auto *result = builder.CreateCall(fn, {obj}, "cancelled");
    return builder.CreateICmpNE(result, llvm::ConstantInt::get(i64_type, 0),
                                "cancelled.bool");
  }
  if (method == "Send" && !node.args.empty())
    return emit_context_send(node, obj);
  if (method == "Exit")
    return emit_context_exit(node, obj);
  return std::nullopt;
}

// The runtime copies the message's bytes from the address it is given: a
// struct or union already arrives as one, anything else is spilled first.
llvm::Value *CodeGen::emit_context_send(const CallExprNode &node,
                                        llvm::Value *obj) {
  auto *val = emit_expr(*node.args[0]);
  if (!val) return nullptr;
  auto *func = builder.GetInsertBlock()->getParent();
  llvm::Value *data_ptr = val;
  auto arg_sem = semantic_type(*node.args[0]);
  bool val_is_ptr_to_payload =
      val->getType()->isPointerTy() && arg_sem &&
      (arg_sem->kind == TypeKind::Struct || arg_sem->kind == TypeKind::Union);
  if (!val_is_ptr_to_payload) {
    auto *tmp = create_entry_alloca(func, "send.tmp", val->getType());
    builder.CreateStore(val, tmp);
    data_ptr = tmp;
  }
  builder.CreateCall(module->getFunction("saga_context_send"),
                     {obj, data_ptr});
  return nullptr;
}

llvm::Value *CodeGen::emit_context_exit(const CallExprNode &node,
                                        llvm::Value *obj) {
  auto *func = builder.GetInsertBlock()->getParent();
  if (!node.args.empty()) {
    auto *val = emit_expr(*node.args[0]);
    if (val) {
      auto *tmp = create_entry_alloca(func, "exit.tmp", val->getType());
      builder.CreateStore(val, tmp);
      uint64_t sz = size_of(val->getType());
      builder.CreateCall(module->getFunction("saga_context_exit"),
                         {obj, tmp, llvm::ConstantInt::get(i64_type, sz)});
    }
  } else {
    auto *null_ptr =
        llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(context));
    builder.CreateCall(module->getFunction("saga_context_exit"),
                       {obj, null_ptr, llvm::ConstantInt::get(i64_type, 0)});
  }
  return nullptr;
}

} // namespace saga

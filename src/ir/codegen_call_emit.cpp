// Copyright 2026 Rob Thornton
// SPDX-License-Identifier: MIT

// Emission of a call to a callee already resolved to an llvm::Function.

#include "ir/codegen.hpp"

#include <llvm/IR/Constants.h>

namespace saga {

llvm::Value *CodeGen::emit_resolved_call(llvm::Function *callee,
                                         const TypePtr &func_type,
                                         const CallExprNode &node) {
  auto &fn_info = std::get<FuncTypeInfo>(func_type->detail);
  std::vector<llvm::Value *> args;

  // Sret lowering: if callee returns a struct via sret, alloca the
  // result struct and pass as the hidden first argument.
  auto *parent_fn = builder.GetInsertBlock()->getParent();
  llvm::Value *sret_slot = nullptr;
  llvm::Type *sret_struct_ty = nullptr;
  if (callee->arg_size() > 0 && callee->getArg(0)->hasStructRetAttr()) {
    sret_struct_ty = callee->getParamStructRetType(0);
    sret_slot = create_entry_alloca(parent_fn, "sret.tmp", sret_struct_ty);
    args.push_back(sret_slot);
  }

  if (fn_info.is_variadic && !fn_info.params.empty()) {
    // Pack variadic arguments: non-variadic params are emitted normally,
    // then the remaining args are packed into a saga_runtime array.
    size_t fixed_count = fn_info.params.size() - 1;
    for (size_t i = 0; i < fixed_count && i < node.args.size(); ++i) {
      auto *val = emit_expr(*node.args[i]);
      if (val) args.push_back(val);
    }
    size_t var_count = node.args.size() > fixed_count
                           ? node.args.size() - fixed_count : 0;
    auto *arr = builder.CreateCall(
        module->getFunction("saga_array_new"),
        {llvm::ConstantInt::get(i64_type, 8),
         llvm::ConstantInt::get(i64_type, var_count)}, "varargs");
    auto *func = builder.GetInsertBlock()->getParent();
    for (size_t i = 0; i < var_count; ++i) {
      auto *val = emit_expr(*node.args[fixed_count + i]);
      if (!val) continue;
      auto *tmp = create_entry_alloca(func, "va.tmp", val->getType());
      builder.CreateStore(val, tmp);
      builder.CreateCall(module->getFunction("saga_array_builder_push"),
                         {arr, tmp});
    }
    args.push_back(arr);
  } else {
    for (size_t i = 0; i < node.args.size(); ++i) {
      auto *val = emit_expr(*node.args[i]);
      if (!val) continue;
      auto param = i < fn_info.params.size()
                       ? unwrap_alias(fn_info.params[i])
                       : nullptr;
      auto arg_sem = semantic_type(*node.args[i]);
      val = coerce_to(val, arg_sem, param);
      // Byval struct/union param: pass pointer to alloca, spill SSA values.
      if (auto *p_ll = byval_param_type(param);
          p_ll && val->getType()->isStructTy()) {
        auto *tmp = create_entry_alloca(parent_fn, "arg.spill", p_ll);
        builder.CreateStore(val, tmp);
        val = tmp;
      }
      args.push_back(val);
    }
  }

  auto *call = builder.CreateCall(callee, args,
      callee->getReturnType()->isVoidTy() ? "" : "pkg.call");

  // Mirror sret/byval attrs on the call site so LLVM lowers correctly.
  unsigned idx = 0;
  if (sret_slot) {
    call->addParamAttr(idx,
        llvm::Attribute::getWithStructRetType(context, sret_struct_ty));
    call->addParamAttr(idx,
        llvm::Attribute::getWithAlignment(context,
            align_of(sret_struct_ty)));
    ++idx;
  }
  for (size_t i = 0; i < fn_info.params.size(); ++i) {
    if (auto *p_ll = byval_param_type(fn_info.params[i])) {
      call->addParamAttr(idx,
          llvm::Attribute::getWithByValType(context, p_ll));
      call->addParamAttr(idx,
          llvm::Attribute::getWithAlignment(context, align_of(p_ll)));
    }
    ++idx;
  }

  if (sret_slot)
    return sret_slot;
  if (callee->getReturnType()->isVoidTy())
    return nullptr;
  return call;
}

llvm::Value *CodeGen::emit_receiver_call(
    llvm::Function *callee, const TypePtr &recv_sem, llvm::Value *recv_value,
    const std::vector<llvm::Value *> &arg_vals, const FuncTypeInfo *method_fi) {
  auto *parent_fn = builder.GetInsertBlock()->getParent();
  std::vector<llvm::Value *> args;

  llvm::Value *sret_slot = nullptr;
  llvm::Type *sret_struct_ty = nullptr;
  if (callee->arg_size() > 0 && callee->getArg(0)->hasStructRetAttr()) {
    sret_struct_ty = callee->getParamStructRetType(0);
    sret_slot = create_entry_alloca(parent_fn, "sret.tmp", sret_struct_ty);
    args.push_back(sret_slot);
  }

  llvm::Value *self = recv_value;
  bool ptr_self = recv_sem && (recv_sem->kind == TypeKind::Struct ||
                               recv_sem->kind == TypeKind::Alias);
  if (ptr_self) {
    auto *self_ll = llvm_type(recv_sem);
    if (self_ll && self_ll->isStructTy() && self->getType() == self_ll) {
      auto *tmp = create_entry_alloca(parent_fn, "self.tmp", self_ll);
      builder.CreateStore(self, tmp);
      self = tmp;
    }
  }
  args.push_back(self);

  auto is_byval_param = [&](size_t i) -> llvm::Type * {
    if (!method_fi || i >= method_fi->params.size())
      return nullptr;
    return byval_param_type(method_fi->params[i]);
  };

  for (size_t i = 0; i < arg_vals.size(); ++i) {
    auto *val = arg_vals[i];
    if (!val)
      continue;
    if (auto *p_ll = is_byval_param(i);
        p_ll && val->getType()->isStructTy()) {
      auto *tmp = create_entry_alloca(parent_fn, "arg.spill", p_ll);
      builder.CreateStore(val, tmp);
      val = tmp;
    }
    args.push_back(val);
  }

  std::string call_name = callee->getReturnType()->isVoidTy() ? "" : "mcall";
  auto *call = builder.CreateCall(callee, args, call_name);

  unsigned cidx = 0;
  if (sret_slot) {
    call->addParamAttr(cidx,
        llvm::Attribute::getWithStructRetType(context, sret_struct_ty));
    call->addParamAttr(cidx, llvm::Attribute::getWithAlignment(
                                 context, align_of(sret_struct_ty)));
    ++cidx;
  }
  ++cidx; // self
  if (method_fi)
    for (size_t i = 0; i < method_fi->params.size(); ++i, ++cidx)
      if (auto *p_ll = is_byval_param(i)) {
        call->addParamAttr(
            cidx, llvm::Attribute::getWithByValType(context, p_ll));
        call->addParamAttr(cidx, llvm::Attribute::getWithAlignment(
                                     context, align_of(p_ll)));
      }

  if (sret_slot)
    return sret_slot;
  if (callee->getReturnType()->isVoidTy())
    return nullptr;
  return call;
}

} // namespace saga

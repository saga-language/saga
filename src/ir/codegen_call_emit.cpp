// Copyright 2026 Rob Thornton
// SPDX-License-Identifier: MIT

// A call, once its arguments are lowered: the one place a call instruction to
// a Saga function is built, so every call carries its callee's ABI.

#include "ir/codegen.hpp"

#include <llvm/IR/Constants.h>

namespace saga {

LoweredSig CodeGen::signature_of(llvm::Function *fn) {
  LoweredSig sig;
  sig.type = fn->getFunctionType();
  if (fn->arg_size() > 0 &&
      fn->hasParamAttribute(0, llvm::Attribute::StructRet))
    sig.sret = fn->getParamStructRetType(0);
  for (auto &arg : fn->args())
    sig.byval.push_back(arg.getParamByValType());
  return sig;
}

// `leading` is the receiver or closure environment, placed after any sret
// slot. An aggregate result is handed back as the address of its slot.
llvm::Value *CodeGen::emit_call(llvm::Value *callee, const LoweredSig &sig,
                                llvm::Value *leading,
                                const std::vector<llvm::Value *> &args) {
  std::vector<llvm::Value *> ll_args;
  llvm::Value *sret_slot = nullptr;
  if (sig.sret) {
    sret_slot = create_entry_alloca(builder.GetInsertBlock()->getParent(),
                                    "sret.tmp", sig.sret);
    ll_args.push_back(sret_slot);
  }
  if (leading)
    ll_args.push_back(leading);
  ll_args.insert(ll_args.end(), args.begin(), args.end());
  for (unsigned i = sret_slot ? 1 : 0;
       i < ll_args.size() && i < sig.type->getNumParams(); ++i)
    ll_args[i] = as_param(ll_args[i], sig.type->getParamType(i));

  auto *call = builder.CreateCall(sig.type, callee, ll_args);
  stamp_abi(call, sig);
  if (sret_slot)
    return sret_slot;
  if (sig.type->getReturnType()->isVoidTy())
    return nullptr;
  call->setName("call");
  return call;
}

// A parameter that takes an aggregate by pointer needs an address, so a value
// computed in registers is given a slot first.
llvm::Value *CodeGen::as_param(llvm::Value *val, llvm::Type *param_ll) {
  if (!param_ll->isPointerTy())
    return val;
  return spill_aggregate(val, "arg.spill");
}

llvm::Value *CodeGen::emit_resolved_call(llvm::Function *callee,
                                         const TypePtr &func_type,
                                         const CallExprNode &node) {
  auto &fn_info = std::get<FuncTypeInfo>(func_type->detail);
  std::vector<llvm::Value *> args;

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
      args.push_back(coerce_to(val, arg_sem, param));
    }
  }
  return emit_call(callee, nullptr, args);
}

llvm::Value *CodeGen::emit_call(llvm::Function *callee, llvm::Value *leading,
                                const std::vector<llvm::Value *> &args) {
  return emit_call(callee, signature_of(callee), leading, args);
}

} // namespace saga

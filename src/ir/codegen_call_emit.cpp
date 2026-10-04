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

const FuncTypeInfo *func_info(const MethodInfo &m) {
  return m.signature && m.signature->kind == TypeKind::Func
             ? &std::get<FuncTypeInfo>(m.signature->detail)
             : nullptr;
}

const FuncTypeInfo *method_signature(const std::vector<MethodInfo> &methods,
                                     const std::string &name) {
  for (auto &m : methods)
    if (m.name == name)
      return func_info(m);
  return nullptr;
}

// The one answer to which parameters own a reference: the caller retains a
// borrowed argument for exactly these, and the callee gives it back. A Task
// is a handle the runtime counts on its own.
bool CodeGen::param_owns_reference(const TypePtr &param) {
  auto shape = unwrap_alias(param);
  if (!shape)
    return false;
  if (shape->kind == TypeKind::String || shape->kind == TypeKind::Array ||
      shape->kind == TypeKind::Map || shape->kind == TypeKind::Interface)
    return true;
  return shape->kind == TypeKind::Struct &&
         std::get<StructTypeInfo>(shape->detail).name != "Task" &&
         owns_managed_fields(shape);
}

// An argument binds the callee's parameter: an error bubbles out of it as out
// of any operand, it takes the parameter's type, and a borrowed value takes
// the reference the parameter's slot will own. A C callee owns nothing.
llvm::Value *CodeGen::emit_argument(const Node &arg, const TypePtr &param,
                                    bool callee_owns) {
  auto *val = emit_operand(arg);
  if (!val)
    return nullptr;
  auto arg_sem = operand_type(arg);
  if (callee_owns && param_owns_reference(param))
    retain_if_borrowed(val, arg_sem, arg);
  return coerce_to(val, arg_sem, unwrap_alias(param));
}

// Arguments past a variadic signature's fixed ones are packed into an array,
// unless a single array of the variadic type is passed through as it is.
std::vector<llvm::Value *>
CodeGen::emit_arguments(const CallExprNode &node, const FuncTypeInfo *fi,
                        bool callee_owns) {
  bool variadic = fi && fi->is_variadic && !fi->params.empty();
  size_t fixed = variadic ? fi->params.size() - 1 : node.args.size();
  std::vector<llvm::Value *> args;
  for (size_t i = 0; i < node.args.size() && i < fixed; ++i) {
    auto param = fi && i < fi->params.size() ? fi->params[i] : nullptr;
    if (auto *val = emit_argument(*node.args[i], param, callee_owns))
      args.push_back(val);
  }
  if (!variadic)
    return args;
  if (auto *packed = pack_variadic_args(node, *fi)) {
    args.push_back(packed);
    return args;
  }
  for (size_t i = fixed; i < node.args.size(); ++i)
    if (auto *val =
            emit_argument(*node.args[i], fi->params.back(), callee_owns))
      args.push_back(val);
  return args;
}

llvm::Value *CodeGen::emit_resolved_call(llvm::Function *callee,
                                         const TypePtr &func_type,
                                         const CallExprNode &node) {
  return emit_call(callee, nullptr,
                   emit_arguments(node, &std::get<FuncTypeInfo>(func_type->detail),
                                  /*callee_owns=*/true));
}

llvm::Value *CodeGen::emit_call(llvm::Function *callee, llvm::Value *leading,
                                const std::vector<llvm::Value *> &args) {
  return emit_call(callee, signature_of(callee), leading, args);
}

} // namespace saga

// Copyright 2026 Rob Thornton
// SPDX-License-Identifier: MIT

// Function values. Every function value is a pointer to a box
// (runtime/box.c) whose vtable has one method, the code to run, which takes
// the box's value as its environment. A closure's box holds a copy of each
// capture; a named function's is a static box whose code ignores it.

#include "ir/codegen.hpp"
#include "util/internal_error.hpp"

#include <llvm/IR/Constants.h>

namespace saga {

namespace {
// After the size, slot operations and writes mask every vtable starts with.
constexpr unsigned kCodeSlot = 3;
} // namespace

llvm::StructType *CodeGen::fn_vtable_type() {
  auto *ptr_type = llvm::PointerType::getUnqual(context);
  return llvm::StructType::get(context,
                               {i64_type, ptr_type, i64_type, ptr_type});
}

// A closure that writes its captures sets the one bit its one method has, so
// a call through a box another name shares copies it first.
llvm::Constant *CodeGen::fn_vtable(const std::string &name,
                                   llvm::Function *code,
                                   const TypePtr &env_sem, bool writes) {
  auto *size = llvm::ConstantInt::get(
      i64_type, env_sem ? size_of(llvm_type(env_sem)) : 0);
  auto *ops = env_sem ? elem_ops_for(env_sem)
                      : llvm::ConstantPointerNull::get(
                            llvm::PointerType::getUnqual(context));
  return new llvm::GlobalVariable(
      *module, fn_vtable_type(), /*isConstant=*/true,
      llvm::GlobalValue::PrivateLinkage,
      llvm::ConstantStruct::get(
          fn_vtable_type(),
          {size, ops, llvm::ConstantInt::get(i64_type, writes ? 1 : 0),
           code}),
      name + ".vtable");
}

// The box owns its copy of each capture, as any binding of it would.
llvm::Value *CodeGen::emit_closure_box(
    const std::string &name, llvm::Function *code, const TypePtr &env_sem,
    const std::vector<Analyzer::CaptureInfo> &captures, bool writes) {
  auto *box = builder.CreateCall(module->getFunction("saga_box_new"),
                                 {fn_vtable(name, code, env_sem, writes)},
                                 "closure");
  if (!env_sem)
    return box;
  auto *env_st = llvm::cast<llvm::StructType>(llvm_type(env_sem));
  auto *env = box_value(box);
  for (size_t i = 0; i < captures.size(); ++i) {
    auto local_it = locals.find(captures[i].name);
    if (local_it == locals.end())
      internal_error("closure captures '" + captures[i].name +
                     "', which is not a local here");
    auto *field_ll = env_st->getElementType(i);
    auto *val = builder.CreateLoad(field_ll, local_it->second,
                                   captures[i].name + ".cap");
    emit_retain(val, unwrap_alias(captures[i].type));
    auto *field =
        builder.CreateStructGEP(env_st, env, i, captures[i].name + ".env");
    builder.CreateStore(val, field);
  }
  return box;
}

// A static box: never counted, so never freed.
llvm::Value *CodeGen::function_value(llvm::Function *fn,
                                     const TypePtr &fn_sem) {
  std::string name = fn->getName().str() + ".fnval";
  if (auto *existing = module->getNamedGlobal(name))
    return existing;
  auto shape = unwrap_alias(fn_sem);
  if (!shape || shape->kind != TypeKind::Func)
    internal_error("function '" + fn->getName().str() +
                   "' is used as a value with no function type");
  auto *code =
      function_value_thunk(fn, std::get<FuncTypeInfo>(shape->detail));
  auto *box_ty = llvm::StructType::get(
      context, {i64_type, llvm::PointerType::getUnqual(context)});
  auto *box = new llvm::GlobalVariable(
      *module, box_ty, /*isConstant=*/false, llvm::GlobalValue::PrivateLinkage,
      llvm::ConstantStruct::get(
          box_ty, {llvm::ConstantInt::getSigned(i64_type, -1),
                   fn_vtable(name, code, nullptr, false)}),
      name);
  box->setAlignment(llvm::Align(16));
  return box;
}

llvm::Function *CodeGen::function_value_thunk(llvm::Function *fn,
                                              const FuncTypeInfo &fi) {
  std::string name = fn->getName().str() + ".thunk";
  auto sig = lower_signature(fi, llvm::PointerType::getUnqual(context));
  auto *thunk = declare_function(name, sig, llvm::Function::PrivateLinkage);
  auto saved = builder.saveIP();
  builder.SetInsertPoint(llvm::BasicBlock::Create(context, "entry", thunk));
  unsigned env = sig.sret ? 1 : 0;
  std::vector<llvm::Value *> args;
  for (auto &arg : thunk->args())
    if (arg.getArgNo() != env)
      args.push_back(&arg);
  if (fn->getFunctionType()->getNumParams() != args.size())
    internal_error("'" + fn->getName().str() + "' does not take the "
                   "arguments its function type passes");
  auto *call = builder.CreateCall(fn, args);
  stamp_abi(call, signature_of(fn));
  if (call->getType()->isVoidTy())
    builder.CreateRetVoid();
  else
    builder.CreateRet(call);
  builder.restoreIP(saved);
  return thunk;
}

llvm::Value *CodeGen::emit_function_value_call(const CallExprNode &node) {
  auto *box = emit_expr(*node.callee);
  if (!box)
    return nullptr;
  return emit_function_value_invoke(*node.callee, box, node);
}

// The call readies the box the way a call through an interface does, as
// method 0: a closure that writes its captures gets a box of its own first.
llvm::Value *CodeGen::emit_function_value_invoke(const Node &callee,
                                                 llvm::Value *box,
                                                 const CallExprNode &node) {
  auto fn_sem = unwrap_alias(semantic_type(callee));
  if (!fn_sem || fn_sem->kind != TypeKind::Func)
    internal_error("a function value is called with no function type");
  auto &fi = std::get<FuncTypeInfo>(fn_sem->detail);
  auto [ready, temporary] = interface_receiver(callee, box, fn_sem, 0);
  auto *ptr_type = llvm::PointerType::getUnqual(context);
  auto *code = builder.CreateLoad(
      ptr_type,
      builder.CreateStructGEP(fn_vtable_type(), box_vtable(ready), kCodeSlot,
                              "fn.code.ptr"),
      "fn.code");
  auto *result = emit_call(code, lower_signature(fi, ptr_type),
                           box_value(ready), emit_arguments(node, &fi, true));
  if (temporary)
    emit_release(ready, fn_sem);
  return result;
}

} // namespace saga

// Copyright 2026 Rob Thornton
// SPDX-License-Identifier: MIT

// A struct method takes its receiver by address, which a call through an
// interface passes as the box's own copy. Any other satisfier's method takes
// its receiver by value, so its vtable entry is a thunk that loads the value
// out of the box and calls the method with the rest of the arguments as they
// came.

#include "ir/codegen.hpp"
#include "util/internal_error.hpp"

namespace saga {

llvm::Function *CodeGen::value_receiver_thunk(const TypePtr &concrete,
                                              const std::string &key,
                                              const std::string &method,
                                              const FuncTypeInfo *sig) {
  std::string name = "saga.thunk." + key + "." + method;
  if (auto *existing = module->getFunction(name))
    return existing;
  if (!sig)
    internal_error("interface method '" + method + "' has no signature for "
                   "the thunk to '" + type_to_string(concrete) + "'");

  auto lowered = lower_signature(*sig, llvm::PointerType::getUnqual(context));
  auto *thunk =
      declare_function(name, lowered, llvm::Function::PrivateLinkage);
  auto saved = builder.saveIP();
  builder.SetInsertPoint(llvm::BasicBlock::Create(context, "entry", thunk));
  unsigned recv = lowered.sret ? 1 : 0;
  auto *value =
      builder.CreateLoad(llvm_type(concrete), thunk->getArg(recv), "recv");
  auto *result = call_value_method(concrete, method, thunk, recv, value);
  if (result)
    builder.CreateRet(result);
  else
    builder.CreateRetVoid();
  builder.restoreIP(saved);
  return thunk;
}

// An enum's Int is its ordinal, which is the value itself.
llvm::Value *CodeGen::call_value_method(const TypePtr &concrete,
                                        const std::string &method,
                                        llvm::Function *thunk, unsigned recv,
                                        llvm::Value *value) {
  auto *callee = value_method_callee(concrete, method);
  if (!callee && unwrap_alias(concrete)->kind == TypeKind::Enum &&
      method == "Int")
    return value;
  if (!callee)
    internal_error("no method '" + method + "' to call on a boxed '" +
                   type_to_string(concrete) + "'");

  std::vector<llvm::Value *> args;
  for (auto &arg : thunk->args())
    args.push_back(arg.getArgNo() == recv ? value
                                          : static_cast<llvm::Value *>(&arg));
  auto *ft = callee->getFunctionType();
  bool fits = ft->getNumParams() == args.size();
  for (unsigned i = 0; fits && i < args.size(); ++i)
    fits = ft->getParamType(i) == args[i]->getType();
  if (!fits)
    internal_error("'" + type_to_string(concrete) + "." + method +
                   "' does not take the arguments its interface passes");

  auto *call = builder.CreateCall(callee, args);
  stamp_abi(call, signature_of(callee));
  return call->getType()->isVoidTy() ? nullptr : call;
}

// An alias's own methods come before the ones of the type it names.
llvm::Function *CodeGen::value_method_callee(const TypePtr &concrete,
                                             const std::string &method) {
  if (concrete->kind == TypeKind::Alias) {
    auto &ai = std::get<AliasTypeInfo>(concrete->detail);
    if (method_signature(ai.methods, method))
      return module->getFunction(
          mangle(type_to_string(concrete) + "__" + method));
    return value_method_callee(ai.underlying, method);
  }
  if (concrete->kind == TypeKind::Enum)
    return enum_method_callee(concrete, method);
  const FuncTypeInfo *fi = nullptr;
  return resolve_member_method_callee(concrete, method, &fi);
}

} // namespace saga

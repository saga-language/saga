// Copyright 2026 Rob Thornton
// SPDX-License-Identifier: MIT

#include "ir/codegen.hpp"
#include "util/internal_error.hpp"

#include <llvm/IR/Constants.h>
#include <llvm/Support/raw_ostream.h>

namespace saga {

namespace {
std::string ll_name(llvm::Type *t) {
  std::string out;
  llvm::raw_string_ostream os(out);
  t->print(os);
  return out;
}
} // namespace

TypePtr CodeGen::declared_return_sem(const NodePtr &return_type) {
  if (!return_type)
    return nullptr;
  if (auto sem = semantic_type(*return_type))
    return sem;
  return lookup_sem_type(*return_type);
}

TypePtr CodeGen::return_sem_of(llvm::Function *func) const {
  auto it = return_sems_.find(func);
  return it == return_sems_.end() ? nullptr : it->second;
}

void CodeGen::emit_fallthrough_return(const BlockNode &block,
                                      llvm::Value *tail_val) {
  auto *tail = block.stmts.empty() ? nullptr : block.stmts.back().get();
  emit_return_value(tail_val, block_result_type(block), tail);
}

// The value is retained and placed before the locals are released, since it
// may well be one of them.
void CodeGen::emit_return_value(llvm::Value *val, const TypePtr &val_sem,
                                const Node *source) {
  auto *func = builder.GetInsertBlock()->getParent();
  auto ret_sem = return_sem_of(func);
  if (val && ret_sem && source)
    retain_if_borrowed(val, val_sem, *source);
  auto *placed = val ? coerce_to(val, val_sem, ret_sem) : nullptr;

  write_back_captures(func);
  if (func->arg_size() > 0 &&
      func->hasParamAttribute(0, llvm::Attribute::StructRet)) {
    if (placed)
      store_into_slot(func->getArg(0), func->getParamStructRetType(0), placed);
    emit_release_locals();
    builder.CreateRetVoid();
    return;
  }

  auto *ret_ll = func->getReturnType();
  auto *out = ret_ll->isVoidTy() ? nullptr : as_return_value(placed, ret_ll);
  emit_release_locals();
  if (out)
    builder.CreateRet(out);
  else
    builder.CreateRetVoid();
}

llvm::Value *CodeGen::as_return_value(llvm::Value *val, llvm::Type *ret_ll) {
  // A body that falls off its end without a value has returned on every path,
  // so this return is unreachable and only has to be well-formed.
  if (!val)
    return llvm::Constant::getNullValue(ret_ll);
  if (val->getType() == ret_ll)
    return val;
  if (ret_ll->isStructTy() && val->getType()->isPointerTy())
    return builder.CreateLoad(ret_ll, val, "ret.val");
  if (ret_ll->isIntegerTy() && val->getType()->isIntegerTy())
    return builder.CreateZExtOrTrunc(val, ret_ll, "ret.int");
  internal_error("a value of LLVM type '" + ll_name(val->getType()) +
                 "' is returned from '" +
                 builder.GetInsertBlock()->getParent()->getName().str() +
                 "', which returns '" + ll_name(ret_ll) + "'");
}

} // namespace saga

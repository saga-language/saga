// Copyright 2026 Rob Thornton
// SPDX-License-Identifier: MIT

#pragma once

#include <llvm/IR/CFG.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>

#include <vector>

namespace saga {

inline std::vector<llvm::CallInst *> calls_to(llvm::Function *fn,
                                       llvm::StringRef callee) {
  std::vector<llvm::CallInst *> out;
  for (auto &bb : *fn)
    for (auto &inst : bb)
      if (auto *call = llvm::dyn_cast<llvm::CallInst>(&inst))
        if (call->getCalledFunction() &&
            call->getCalledFunction()->getName().contains(callee))
          out.push_back(call);
  return out;
}

inline bool is_call_to(const llvm::User *user, llvm::StringRef callee) {
  auto *call = llvm::dyn_cast<llvm::CallInst>(user);
  return call && call->getCalledFunction() &&
         call->getCalledFunction()->getName().contains(callee);
}

// Whether `val` is stored into a slot whose contents are later handed to a
// call to `releaser`.
inline bool released_through_slot(llvm::Value *val, llvm::StringRef releaser) {
  for (auto *user : val->users()) {
    auto *store = llvm::dyn_cast<llvm::StoreInst>(user);
    if (!store || store->getValueOperand() != val)
      continue;
    for (auto *slot_user : store->getPointerOperand()->users())
      if (auto *load = llvm::dyn_cast<llvm::LoadInst>(slot_user))
        for (auto *load_user : load->users())
          if (is_call_to(load_user, releaser))
            return true;
  }
  return false;
}

inline llvm::AllocaInst *local_slot(llvm::Function *fn, llvm::StringRef name) {
  for (auto &inst : fn->getEntryBlock())
    if (auto *slot = llvm::dyn_cast<llvm::AllocaInst>(&inst))
      if (slot->getName() == name)
        return slot;
  return nullptr;
}

// The calls to `callee` that take the value loaded from `slot`.
inline std::vector<llvm::CallInst *> calls_on_slot(llvm::Value *slot,
                                            llvm::StringRef callee) {
  std::vector<llvm::CallInst *> out;
  for (auto *user : slot->users())
    if (auto *load = llvm::dyn_cast<llvm::LoadInst>(user))
      for (auto *load_user : load->users())
        if (is_call_to(load_user, callee))
          out.push_back(llvm::cast<llvm::CallInst>(load_user));
  return out;
}

inline bool branches_to(llvm::BasicBlock *bb, llvm::StringRef prefix) {
  for (auto *succ : llvm::successors(bb))
    if (succ->getName().starts_with(prefix))
      return true;
  return false;
}

inline bool released_directly(llvm::Value *val, llvm::StringRef releaser) {
  for (auto *user : val->users())
    if (is_call_to(user, releaser))
      return true;
  return false;
}


} // namespace saga

// Copyright 2026 Rob Thornton
// SPDX-License-Identifier: MIT

// A call on a union receiver, whose callee is chosen at run time by its tag.
// An interface receiver's is found through its vtable (codegen_interfaces.cpp).

#include "ir/codegen.hpp"

#include <llvm/IR/Constants.h>

namespace saga {

llvm::Value *CodeGen::emit_union_method_dispatch(const CallExprNode &node,
                                                 const std::string &method,
                                                 const TypePtr &union_sem,
                                                 llvm::Value *union_ptr) {
  auto &alts = std::get<UnionTypeInfo>(union_sem->detail).alternatives;

  // Every member satisfies one interface declaring `method`, so the arguments
  // are lowered once, against any member's signature, and must not re-run
  // their side effects per arm.
  const FuncTypeInfo *shared_sig = nullptr;
  if (!alts.empty())
    resolve_member_method_callee(alts.front(), method, &shared_sig);
  auto arg_vals = emit_arguments(node, shared_sig, true);

  auto *func = builder.GetInsertBlock()->getParent();
  auto *union_st = get_union_llvm_type(union_sem);
  auto *i8_ty = llvm::Type::getInt8Ty(context);

  auto *tag_gep =
      builder.CreateStructGEP(union_st, union_ptr, 0, "um.tag.ptr");
  auto *tag_val = builder.CreateLoad(i8_ty, tag_gep, "um.tag");

  auto *default_bb = llvm::BasicBlock::Create(context, "um.default");
  auto *merge_bb = llvm::BasicBlock::Create(context, "um.merge");
  auto *sw = builder.CreateSwitch(tag_val, default_bb, alts.size());

  struct ArmResult {
    llvm::Value *value;
    llvm::BasicBlock *block;
    bool terminated;
  };
  std::vector<ArmResult> results;

  for (size_t i = 0; i < alts.size(); ++i) {
    auto &alt = alts[i];
    auto *case_bb = llvm::BasicBlock::Create(
        context, "um.case." + std::to_string(i), func);
    int tag = union_tag_for_type(alt, union_sem);
    sw->addCase(llvm::ConstantInt::get(i8_ty, tag >= 0 ? tag : (int)i),
                case_bb);

    builder.SetInsertPoint(case_bb);
    auto *recv = emit_union_extract(union_ptr, alt, union_sem);
    const FuncTypeInfo *m_fi = nullptr;
    auto *callee = resolve_member_method_callee(alt, method, &m_fi);
    llvm::Value *result =
        callee ? emit_call(callee, recv, arg_vals)
               : nullptr;

    bool terminated = builder.GetInsertBlock()->getTerminator() != nullptr;
    if (!terminated)
      builder.CreateBr(merge_bb);
    results.push_back({result, builder.GetInsertBlock(), terminated});
  }

  // The tag is always one of the members, so the default is unreachable.
  func->insert(func->end(), default_bb);
  builder.SetInsertPoint(default_bb);
  builder.CreateUnreachable();

  func->insert(func->end(), merge_bb);
  builder.SetInsertPoint(merge_bb);

  llvm::Type *phi_type = nullptr;
  bool all_have_value = true;
  for (auto &r : results) {
    if (!r.value || r.terminated)
      all_have_value = false;
    else if (!phi_type)
      phi_type = r.value->getType();
    else if (r.value->getType() != phi_type)
      all_have_value = false;
  }
  if (all_have_value && phi_type && !phi_type->isVoidTy()) {
    auto *phi = builder.CreatePHI(phi_type, results.size(), "um.val");
    for (auto &r : results)
      if (!r.terminated && r.value)
        phi->addIncoming(r.value, r.block);
    return phi;
  }
  return nullptr;
}

} // namespace saga

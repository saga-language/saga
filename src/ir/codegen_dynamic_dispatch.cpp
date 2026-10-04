// Copyright 2026 Rob Thornton
// SPDX-License-Identifier: MIT

// Calls whose callee is chosen at run time: a union receiver by its tag, an
// interface receiver through its vtable.

#include "ir/codegen.hpp"

#include <llvm/IR/Constants.h>

namespace saga {

llvm::Value *CodeGen::emit_union_method_dispatch(const CallExprNode &node,
                                                 const std::string &method,
                                                 const TypePtr &union_sem,
                                                 llvm::Value *union_ptr) {
  auto &alts = std::get<UnionTypeInfo>(union_sem->detail).alternatives;

  // Emit call arguments once — they are identical for every arm and must not
  // re-run their side effects per member.
  std::vector<llvm::Value *> arg_vals;
  for (auto &arg_node : node.args)
    if (auto *v = emit_expr(*arg_node))
      arg_vals.push_back(v);

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

llvm::Value *CodeGen::emit_interface_dispatch(const CallExprNode &node,
                                              const SelectorNode &sel,
                                              const std::string &method,
                                              const TypePtr &obj_sem,
                                              llvm::Value *obj) {
  auto &iface_info = std::get<InterfaceTypeInfo>(obj_sem->detail);
  // key_for falls back to package_name for local types; bare "Error" stays
  // bare since its origin_package is empty and the built-in uses bare key.
  std::string iface_key = key_for(iface_info.origin_package, iface_info.name);

  auto mn_it = iface_method_names.find(iface_key);
  auto vt_it = iface_vtable_types.find(iface_key);
  if (mn_it == iface_method_names.end() ||
      vt_it == iface_vtable_types.end())
    return nullptr;

  auto &methods = mn_it->second;
  auto *vtable_st = vt_it->second;

  int method_idx = -1;
  for (size_t i = 0; i < methods.size(); ++i) {
    if (methods[i] == method) {
      method_idx = static_cast<int>(i);
      break;
    }
  }
  if (method_idx < 0)
    return nullptr;

  auto *ptr_type = llvm::PointerType::getUnqual(context);

  // If obj is an identifier, get the alloca for the fat pointer.
  llvm::Value *fat_ptr = obj;
  if (auto *id = std::get_if<IdentifierNode>(&sel.object->data)) {
    auto local_it = locals.find(std::string(id->name));
    if (local_it != locals.end()) {
      auto *alloca = local_it->second;
      if (alloca->getAllocatedType() == iface_fat_ptr_type) {
        fat_ptr = alloca;
      } else if (alloca->getAllocatedType()->isPointerTy()) {
        fat_ptr = builder.CreateLoad(ptr_type, alloca, "fat.load");
      }
    }
  }

  auto *data_gep = builder.CreateStructGEP(
      iface_fat_ptr_type, fat_ptr, 0, "iface.data.ptr");
  auto *data_ptr = builder.CreateLoad(ptr_type, data_gep, "data");

  auto *vtable_gep = builder.CreateStructGEP(
      iface_fat_ptr_type, fat_ptr, 1, "iface.vtable.ptr");
  auto *vtable_ptr = builder.CreateLoad(ptr_type, vtable_gep, "vtable");

  auto *fn_gep = builder.CreateStructGEP(
      vtable_st, vtable_ptr, method_idx, "vfn.ptr");
  auto *fn_ptr = builder.CreateLoad(ptr_type, fn_gep, "vfn");

  const FuncTypeInfo *fi = nullptr;
  for (auto &im : iface_info.methods)
    if (im.name == method && im.signature &&
        im.signature->kind == TypeKind::Func) {
      fi = &std::get<FuncTypeInfo>(im.signature->detail);
      break;
    }
  if (!fi)
    return nullptr;

  // The vtable holds the concrete type's method itself, so the call takes
  // the ABI that method was declared with: the data pointer as its receiver.
  std::vector<llvm::Value *> args;
  for (auto &arg_node : node.args)
    if (auto *val = emit_expr(*arg_node))
      args.push_back(val);
  return emit_call(fn_ptr, lower_signature(*fi, ptr_type), data_ptr, args);
}

} // namespace saga

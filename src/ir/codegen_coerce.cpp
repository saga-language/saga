// Copyright 2026 Rob Thornton
// SPDX-License-Identifier: MIT

#include "ir/codegen.hpp"

namespace saga {

llvm::Value *CodeGen::coerce_to(llvm::Value *val, const TypePtr &from,
                                const TypePtr &to) {
  auto target = unwrap_alias(to);
  if (!val || !from || !target)
    return val;

  llvm::Value *placed = nullptr;
  if (target->kind == TypeKind::Union)
    placed = as_union_ptr(val, unwrap_alias(from), target);
  else if (target->kind == TypeKind::Interface)
    placed = as_interface_ptr(val, unwrap_alias(from), target);
  return placed ? placed : val;
}

// Never adopts storage the value already has: that may be another local's slot.
llvm::AllocaInst *CodeGen::bind_local(const std::string &name,
                                      llvm::Value *val, const TypePtr &sem) {
  auto *slot_ll = local_slot_type(sem, val);
  auto *func = builder.GetInsertBlock()->getParent();
  auto *slot = create_entry_alloca(func, name, slot_ll);
  if (val)
    store_into_slot(slot, slot_ll, val);
  locals[name] = slot;
  track_managed(slot, unwrap_alias(sem));
  return slot;
}

// A local holds a closure or an interface as the fat pair itself; llvm_type
// answers with the pointer to one that a parameter receives.
llvm::Type *CodeGen::local_slot_type(const TypePtr &sem, llvm::Value *val) {
  auto *held = llvm::dyn_cast_or_null<llvm::AllocaInst>(val);
  if (held && held->getAllocatedType() == closure_fat_ptr_type)
    return closure_fat_ptr_type;
  auto s = unwrap_alias(sem);
  if (s && s->kind == TypeKind::Interface)
    return iface_fat_ptr_type;
  return storage_type(s);
}

llvm::Value *CodeGen::as_union_ptr(llvm::Value *val, const TypePtr &val_sem,
                                   const TypePtr &union_sem) {
  auto *union_ll = get_union_llvm_type(union_sem);
  if (!val || !union_ll)
    return nullptr;
  if (val_sem && val_sem->kind == TypeKind::Union) {
    val = spill_aggregate(val, "union.spill");
    if (types_equal(val_sem, union_sem))
      return val;
    return emit_union_convert(val, val_sem, union_sem);
  }
  return emit_union_wrap(val, materialize_untyped(val_sem), union_sem);
}

llvm::Value *CodeGen::as_interface_ptr(llvm::Value *val,
                                       const TypePtr &val_sem,
                                       const TypePtr &iface_sem) {
  if (!val_sem || val_sem->kind != TypeKind::Struct)
    return nullptr;
  return emit_interface_box(spill_aggregate(val, "iface.spill"), val_sem,
                            iface_sem);
}

} // namespace saga

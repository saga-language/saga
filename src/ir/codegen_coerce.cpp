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

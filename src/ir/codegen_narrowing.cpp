// Copyright 2026 Rob Thornton
// SPDX-License-Identifier: MIT

// A narrowed name: for one branch, a view of its union's payload as the type
// an `is` test found. The variable is still the union, so a write that starts
// from the view lands in the union, and every view of it follows.

#include "ir/codegen.hpp"

#include <llvm/IR/Constants.h>

namespace saga {

llvm::AllocaInst *CodeGen::narrow_local(const std::string &name,
                                        const TypePtr &from,
                                        const TypePtr &to) {
  auto it = locals.find(name);
  if (it == locals.end() || !to)
    return nullptr;

  auto *displaced = it->second;
  auto outer = narrowed_slots_.find(displaced);
  auto origin = outer != narrowed_slots_.end() ? outer->second.origin
                                               : NarrowOrigin{displaced, from};
  auto *func = builder.GetInsertBlock()->getParent();
  auto *view = create_entry_alloca(func, name + ".narrowed", storage_type(to));
  narrowed_slots_[view] = {to, origin};
  narrow_views_[origin.slot].push_back(view);
  fill_view(view);
  locals[name] = view;
  return displaced;
}

void CodeGen::end_narrowing(const std::string &name,
                            llvm::AllocaInst *displaced) {
  auto *view = locals[name];
  narrow_views_[narrowed_slots_.at(view).origin.slot].pop_back();
  locals[name] = displaced;
}

// More than one alternative surviving leaves a union — a narrower one, with
// its own tag numbering.
void CodeGen::fill_view(llvm::AllocaInst *view) {
  auto &[sem, origin] = narrowed_slots_.at(view);
  if (sem->kind == TypeKind::Union)
    store_into_slot(view, view->getAllocatedType(),
                    emit_union_convert(origin.slot, origin.sem, sem));
  else
    builder.CreateStore(emit_union_extract(origin.slot, sem, origin.sem), view);
}

void CodeGen::store_view(llvm::AllocaInst *view) {
  auto &[sem, origin] = narrowed_slots_.at(view);
  if (sem->kind == TypeKind::Union)
    store_into_slot(origin.slot, origin.slot->getAllocatedType(),
                    emit_union_convert(view, sem, origin.sem));
  else
    builder.CreateStore(builder.CreateLoad(view->getAllocatedType(), view),
                        alternative_address(origin.slot, sem, origin.sem));
}

llvm::AllocaInst *CodeGen::narrowed_root(const Node &target) {
  auto it = locals.find(std::string(binding_root(target)));
  if (it == locals.end() || !narrowed_slots_.count(it->second))
    return nullptr;
  return it->second;
}

// The view holds the union's references for the length of the write, so what
// the write replaces it releases on the union's behalf. A boxed alternative's
// box can be shared by another copy of the union, so the union takes one of
// its own first.
llvm::AllocaInst *CodeGen::begin_view_write(const Node &target) {
  auto *view = narrowed_root(target);
  if (view && union_alt_is_boxed(narrowed_slots_.at(view).sem))
    unshare_box(narrowed_slots_.at(view));
  return view;
}

void CodeGen::end_view_write(llvm::AllocaInst *view) {
  if (!view)
    return;
  store_view(view);
  for (auto *each : narrow_views_[narrowed_slots_.at(view).origin.slot])
    fill_view(each);
}

void CodeGen::unshare_box(const NarrowedSlot &narrowed) {
  auto &[sem, origin] = narrowed;
  auto *payload = builder.CreateStructGEP(get_union_llvm_type(origin.sem),
                                          origin.slot, 1, "union.payload");
  auto *box = builder.CreateLoad(llvm::PointerType::getUnqual(context),
                                 payload, "union.box");
  auto *size = llvm::ConstantInt::get(i64_type, size_of(llvm_type(sem)));
  builder.CreateStore(
      builder.CreateCall(module->getFunction("saga_shared_make_unique"),
                         {box, size}, "box.uniq"),
      payload);
  builder.CreateCall(module->getFunction("saga_shared_release"), {box});
}

} // namespace saga

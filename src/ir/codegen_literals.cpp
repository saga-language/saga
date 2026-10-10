// Copyright 2026 Rob Thornton
// SPDX-License-Identifier: MIT

// Composite literals: arrays, maps and structs. Error values are struct
// literals too; what only they need is in codegen_error_literals.cpp.

#include "ir/codegen.hpp"

#include <llvm/IR/Constants.h>

#include <algorithm>
#include <cstdint>

namespace saga {

// ===========================================================================
// Array literals
// ===========================================================================

int64_t CodeGen::element_size_of(llvm::Type *ll) {
  if (!ll)
    return 8;
  if (ll->isStructTy())
    return size_of(ll);
  return ll->isIntegerTy(1) ? 1 : 8;
}

// The literal's own type, not the first element's: an element that is one
// alternative of a union element type says nothing about how wide the slot is.
// An empty literal answers `unknown`, which nothing is stored through anyway.
TypePtr CodeGen::collection_slot_type(const Node &parent, Slot slot,
                                      const Node *fallback) {
  auto sem = unwrap_alias(semantic_type(parent));
  TypePtr part;
  if (sem && sem->kind == TypeKind::Array && slot == Slot::Element)
    part = std::get<ArrayTypeInfo>(sem->detail).element;
  else if (sem && sem->kind == TypeKind::Map) {
    auto &info = std::get<MapTypeInfo>(sem->detail);
    part = slot == Slot::Key ? info.key : info.value;
  } else if (fallback)
    part = operand_type(*fallback);

  if (part && part->kind == TypeKind::Unknown)
    return nullptr;
  return part;
}

// The runtime copies a fixed number of bytes from a void*, so every element is
// written to an address first. A union slot takes its alternative through the
// wrap so the tag is set, exactly as a union struct field does.
llvm::Value *CodeGen::collection_slot_address(llvm::Type *slot_ll,
                                              const TypePtr &slot_sem,
                                              llvm::Value *val,
                                              const TypePtr &val_sem) {
  if (!val)
    return nullptr;

  val = coerce_to(val, val_sem, slot_sem);

  if (slot_ll && slot_ll->isStructTy())
    return spill_aggregate(val, "elem.tmp");

  auto *func = builder.GetInsertBlock()->getParent();
  auto *tmp = create_entry_alloca(func, "elem.tmp", val->getType());
  builder.CreateStore(val, tmp);
  return tmp;
}

llvm::Value *CodeGen::emit_array_literal(const ArrayLiteralNode &node,
                                         const Node &parent) {
  auto elem_sem = collection_slot_type(
      parent, Slot::Element,
      node.elements.empty() ? nullptr : node.elements[0].get());
  auto *arr = emit_new_array(elem_sem, node.elements.size(), "arr");
  for (auto &elem_node : node.elements)
    emit_push_element(arr, elem_sem, *elem_node);
  return arr;
}

void CodeGen::fill_range(llvm::Value *arr, llvm::Value *low,
                         llvm::Value *high) {
  auto *func = builder.GetInsertBlock()->getParent();
  auto *elem_ll = low->getType();
  auto *cur = create_entry_alloca(func, "range.cur", elem_ll);
  auto *slot = create_entry_alloca(func, "range.slot", elem_ll);
  builder.CreateStore(low, cur);

  auto *cond_bb = llvm::BasicBlock::Create(context, "range.cond", func);
  auto *body_bb = llvm::BasicBlock::Create(context, "range.body", func);
  auto *done_bb = llvm::BasicBlock::Create(context, "range.done", func);

  builder.CreateBr(cond_bb);
  builder.SetInsertPoint(cond_bb);
  auto *v = builder.CreateLoad(elem_ll, cur, "range.v");
  builder.CreateCondBr(builder.CreateICmpSLT(v, high, "range.cmp"), body_bb,
                       done_bb);

  builder.SetInsertPoint(body_bb);
  builder.CreateStore(v, slot);
  builder.CreateCall(module->getFunction("saga_array_builder_push"),
                     {arr, slot});
  builder.CreateStore(
      builder.CreateAdd(v, llvm::ConstantInt::get(elem_ll, 1), "range.next"),
      cur);
  builder.CreateBr(cond_bb);

  builder.SetInsertPoint(done_bb);
}

// `[0..10]` builds the array its bounds describe.
llvm::Value *CodeGen::emit_range_literal(const RangeNode &node) {
  auto *low = emit_expr(*node.low);
  auto *high = emit_expr(*node.high);
  if (!low || !high)
    return nullptr;

  auto *arr = builder.CreateCall(
      module->getFunction("saga_array_new"),
      {llvm::ConstantInt::get(i64_type, element_size_of(low->getType())),
       llvm::ConstantInt::get(i64_type, 4),
       llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(context))},
      "range.arr");
  fill_range(arr, low, high);
  return arr;
}

// ===========================================================================
// Map literals
// ===========================================================================

llvm::Value *CodeGen::emit_map_literal(const MapLiteralNode &node,
                                       const Node &parent) {
  const Node *first_key =
      node.entries.empty() ? nullptr : node.entries[0].key.get();
  const Node *first_val =
      node.entries.empty() ? nullptr : node.entries[0].value.get();
  auto key_sem = collection_slot_type(parent, Slot::Key, first_key);
  auto val_sem = collection_slot_type(parent, Slot::Value, first_val);
  auto *map = emit_new_map(key_sem, val_sem);
  for (auto &entry : node.entries)
    emit_set_entry(map, key_sem, val_sem, entry);
  return map;
}

// ===========================================================================
// Struct literals
// ===========================================================================

llvm::Value *CodeGen::emit_struct_literal(const StructLiteralNode &node,
                                          const Node &parent) {
  // Prefer the analyzer's recorded type for the literal expression — for
  // a generic struct literal like `Box{value: Point{...}}` the parent
  // node carries the instantiation `Box<Point>` (with substituted fields
  // and concrete type_args), whereas `node.type_expr` is just the bare
  // template identifier.  Fall back to the type-expr type for older
  // call sites or non-generic structs.
  auto sem = semantic_type(parent);
  if (!sem || sem->kind != TypeKind::Struct)
    sem = semantic_type(*node.type_expr);
  if (!sem || sem->kind != TypeKind::Struct)
    return nullptr;

  auto &info = std::get<StructTypeInfo>(sem->detail);
  // Materialize the per-instantiation LLVM struct on first use; for
  // non-generic structs this is a cache hit on the type registered by
  // emit_struct_decl/materialize_import.
  llvm_type(sem);
  std::string skey = struct_cache_key(info);
  auto st_it = struct_types.find(skey);
  if (st_it == struct_types.end())
    return nullptr;

  auto *st = st_it->second;
  auto *func = builder.GetInsertBlock()->getParent();

  // A field-less error with a constant message is immutable and identical
  // across constructions, so it shares one rodata global (no heap allocation).
  if (info.is_error && info.fields.size() == 2)
    if (auto msg = const_error_message(node, info))
      return emit_error_singleton(info, *msg);

  // Errors are boxed on the heap (they escape via unions/returns); other
  // structs get a zero-initialised stack alloca.
  llvm::Value *storage = nullptr;
  if (info.is_error) {
    storage = emit_error_box(sem, st);
  } else {
    storage = create_entry_alloca(func, info.name + ".lit", st);
    builder.CreateStore(llvm::Constant::getNullValue(st), storage);
  }

  // Apply comptime field defaults first (descending embed slots), then the
  // explicit literal fields, which override. Defaults are side-effect-free
  // comptime expressions, so a defaulted-then-provided field just lowers the
  // default and overwrites it.
  apply_struct_field_defaults(storage, sem);

  // Store each provided field. For a literal that addresses a promoted field
  // (the field lives on an embedded struct), struct_field_gep walks the embed
  // layout and hands back a pointer into the right inner slot.
  for (auto &fa : node.fields) {
    std::string fname(fa.name.name);
    auto [gep, field_ll] = struct_field_gep(storage, sem, fname);
    if (!gep)
      continue;
    TypePtr field_sem;
    for (auto &fi : info.fields)
      if (fi.name == fname) { field_sem = fi.type; break; }
    store_struct_field(gep, field_ll, field_sem, *fa.value);
  }

  if (info.is_error) {
    bool msg_provided = false;
    for (auto &fa : node.fields)
      if (fa.name.name == "message") { msg_provided = true; break; }
    if (!msg_provided)
      emit_error_message_default(storage, sem, info);
  }

  // For a boxed error this is the heap box pointer; for a struct, the alloca.
  return storage;
}

void CodeGen::store_struct_field(llvm::Value *gep, llvm::Type *field_ll,
                                 const TypePtr &field_sem,
                                 const Node &value_node) {
  auto *val = emit_operand(value_node);
  if (!val)
    return;
  retain_if_borrowed(val, unwrap_alias(operand_type(value_node)), value_node);
  val = coerce_to(val, operand_type(value_node), field_sem);

  // D1: aggregate fields are stored inline. If the rhs is a pointer to a
  // struct (e.g. from a nested struct literal), memcpy the bytes rather than
  // storing the pointer into the struct slot.
  if (field_ll && field_ll->isStructTy() && val->getType()->isPointerTy()) {
    uint64_t sz = size_of(field_ll);
    llvm::Align al = align_of(field_ll);
    builder.CreateMemCpy(gep, al, val, al, sz);
  } else {
    builder.CreateStore(val, gep);
  }
}

void CodeGen::apply_struct_field_defaults(llvm::Value *struct_ptr,
                                          const TypePtr &struct_sem) {
  auto &info = std::get<StructTypeInfo>(struct_sem->detail);
  std::string skey = struct_cache_key(info);
  auto st_it = struct_types.find(skey);
  if (st_it == struct_types.end())
    return;
  auto *st = st_it->second;

  for (size_t i = 0; i < info.fields.size() && i < st->getNumElements(); ++i) {
    if (!info.fields[i].default_value)
      continue;
    // An error's message default may interpolate sibling fields, so it is
    // emitted last (emit_error_message_default) once they are populated.
    if (info.is_error && info.fields[i].name == "message")
      continue;
    auto *gep = builder.CreateStructGEP(st, struct_ptr, i, info.fields[i].name);
    llvm::Type *field_ll = st->getElementType(i);
    if (info.fields[i].type)
      if (auto *sem_ll = llvm_type(info.fields[i].type))
        field_ll = sem_ll;
    store_struct_field(gep, field_ll, info.fields[i].type,
                       *info.fields[i].default_value);
  }

  for (size_t ei = 0; ei < info.embeds.size(); ++ei) {
    auto &embed = info.embeds[ei];
    if (!embed || embed->kind != TypeKind::Struct)
      continue;
    size_t slot = info.fields.size() + ei;
    if (slot >= st->getNumElements())
      break;
    auto &einfo = std::get<StructTypeInfo>(embed->detail);
    auto *eslot =
        builder.CreateStructGEP(st, struct_ptr, slot, embed_slot_name(einfo));
    apply_struct_field_defaults(eslot, embed);
  }
}

} // namespace saga

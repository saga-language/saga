// Copyright 2026 Rob Thornton
// SPDX-License-Identifier: MIT

// Arrays and maps own what their elements point at. The runtime retains what
// it stores and releases what it drops, through slot operations codegen hands
// it per element type when the collection is made; so a value made only to be
// stored is released once the collection holds it.

#include "ir/codegen.hpp"
#include "util/internal_error.hpp"

#include <llvm/IR/Constants.h>

namespace saga {

// A counted kind's slot holds its pointer, and a struct's holds its fields.
// A union or an interface element holds references nothing counts yet.
bool CodeGen::slot_holds_references(const TypePtr &sem) {
  auto s = unwrap_alias(sem);
  if (!s)
    return false;
  return s->kind == TypeKind::String || s->kind == TypeKind::Array ||
         s->kind == TypeKind::Map || owns_managed_fields(s);
}

llvm::Constant *CodeGen::elem_ops_for(const TypePtr &sem) {
  auto s = unwrap_alias(sem);
  if (!slot_holds_references(s))
    return llvm::ConstantPointerNull::get(
        llvm::PointerType::getUnqual(context));
  switch (s->kind) {
  case TypeKind::String:
    return runtime_elem_ops("saga_string_elem_ops");
  case TypeKind::Array:
    return runtime_elem_ops("saga_array_elem_ops");
  case TypeKind::Map:
    return runtime_elem_ops("saga_map_elem_ops");
  default:
    return struct_elem_ops(s);
  }
}

llvm::StructType *CodeGen::elem_ops_type() {
  auto *ptr_ty = llvm::PointerType::getUnqual(context);
  return llvm::StructType::get(context, {ptr_ty, ptr_ty});
}

llvm::Constant *CodeGen::runtime_elem_ops(const std::string &name) {
  if (auto *existing = module->getNamedGlobal(name))
    return existing;
  return new llvm::GlobalVariable(*module, elem_ops_type(), /*isConstant=*/true,
                                  llvm::GlobalValue::ExternalLinkage, nullptr,
                                  name);
}

llvm::Constant *CodeGen::struct_elem_ops(const TypePtr &sem) {
  auto &info = std::get<StructTypeInfo>(sem->detail);
  std::string name = struct_cache_key(info) + "__elem_ops";
  if (auto *existing = module->getNamedGlobal(name))
    return existing;
  auto *retain = struct_ownership_fn(sem, true);
  auto *release = struct_ownership_fn(sem, false);
  if (!retain || !release)
    internal_error("struct '" + info.name + "' is stored in a collection but "
                   "has no lowered layout to walk its fields by");
  return new llvm::GlobalVariable(
      *module, elem_ops_type(), /*isConstant=*/true,
      llvm::GlobalValue::PrivateLinkage,
      llvm::ConstantStruct::get(elem_ops_type(), {retain, release}), name);
}

llvm::Value *CodeGen::emit_new_array(const TypePtr &elem_sem, int64_t cap,
                                     const std::string &name) {
  auto *elem_ll = elem_sem ? llvm_type(elem_sem) : i64_type;
  return builder.CreateCall(
      module->getFunction("saga_array_new"),
      {llvm::ConstantInt::get(i64_type, element_size_of(elem_ll)),
       llvm::ConstantInt::get(i64_type, std::max<int64_t>(cap, 4)),
       elem_ops_for(elem_sem)},
      name);
}

llvm::Value *CodeGen::emit_new_map(const TypePtr &key_sem,
                                   const TypePtr &val_sem) {
  auto *key_ll = key_sem ? llvm_type(key_sem) : i64_type;
  auto *val_ll = val_sem ? llvm_type(val_sem) : i64_type;
  return builder.CreateCall(
      module->getFunction("saga_map_new"),
      {llvm::ConstantInt::get(i64_type, element_size_of(key_ll)),
       llvm::ConstantInt::get(i64_type, element_size_of(val_ll)),
       llvm::ConstantInt::get(i64_type,
                              static_cast<int64_t>(key_kind_for(key_sem))),
       get_or_emit_key_ops(key_sem), elem_ops_for(key_sem),
       elem_ops_for(val_sem)},
      "map");
}

void CodeGen::emit_push_element(llvm::Value *arr, const TypePtr &elem_sem,
                                const Node &node) {
  auto *val = emit_operand(node);
  auto *src = collection_slot_address(llvm_type(elem_sem), elem_sem, val,
                                      operand_type(node));
  if (!src)
    return;
  builder.CreateCall(module->getFunction("saga_array_builder_push"),
                     {arr, src});
  settle_stored(val, operand_type(node), node, elem_sem);
}

void CodeGen::emit_set_entry(llvm::Value *map, const TypePtr &key_sem,
                             const TypePtr &val_sem,
                             const KeyValueNode &entry) {
  auto *key = emit_operand(*entry.key);
  auto *val = emit_operand(*entry.value);
  auto *key_src = collection_slot_address(llvm_type(key_sem), key_sem, key,
                                          operand_type(*entry.key));
  auto *val_src = collection_slot_address(llvm_type(val_sem), val_sem, val,
                                          operand_type(*entry.value));
  if (!key_src || !val_src)
    return;
  builder.CreateCall(module->getFunction("saga_map_set"),
                     {map, key_src, val_src});
  settle_stored(key, operand_type(*entry.key), *entry.key, key_sem);
  settle_stored(val, operand_type(*entry.value), *entry.value, val_sem);
}

// Called once the collection holds `val`. A slot whose type has no slot
// operations holds what it is given without counting it, so a borrowed value
// takes a reference for it instead, which leaks rather than dangles.
void CodeGen::settle_stored(llvm::Value *val, const TypePtr &val_sem,
                            const Node &source, const TypePtr &slot_sem) {
  if (!val)
    return;
  if (slot_holds_references(slot_sem))
    release_handed_over(val, source, slot_sem);
  else
    retain_if_borrowed(val, unwrap_alias(val_sem), source);
}

// The collection took a reference of its own or did not keep the value, so
// one made for the call is released either way.
void CodeGen::release_handed_over(llvm::Value *val, const Node &source,
                                  const TypePtr &slot_sem) {
  if (val && slot_holds_references(slot_sem) &&
      value_ownership(source) == Ownership::Owned)
    emit_release(val, unwrap_alias(slot_sem));
}

} // namespace saga

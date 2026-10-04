// Copyright 2026 Rob Thornton
// SPDX-License-Identifier: MIT

// Interface values. An interface value is a pointer to a counted heap box
// holding its own copy of the value and the vtable its methods are found by
// (runtime/box.c). Every vtable starts with the value's size, its slot
// operations and a mask of the methods that write through their receiver, so
// the runtime copies or frees a box without knowing what it holds.

#include "ir/codegen.hpp"
#include "util/internal_error.hpp"

#include <llvm/IR/Constants.h>

namespace saga {

namespace {
// size, slot operations, writes mask
constexpr unsigned kVtablePrefix = 3;
// Matches SAGA_RUNTIME_BOX_VALUE_OFFSET: { i64 refcount, ptr vtable }.
constexpr uint64_t kBoxValueOffset = 16;
constexpr unsigned kBoxVtableField = 1;
} // namespace

bool boxes_into(const TypePtr &val_sem, const TypePtr &slot_sem) {
  auto slot = unwrap_alias(slot_sem);
  auto val = unwrap_alias(val_sem);
  return slot && val && slot->kind == TypeKind::Interface &&
         val->kind != TypeKind::Interface;
}

void CodeGen::declare_vtable_type(const std::string &key,
                                  const std::vector<MethodInfo> &methods) {
  auto *ptr_type = llvm::PointerType::getUnqual(context);
  std::vector<llvm::Type *> fields{i64_type, ptr_type, i64_type};
  std::vector<std::string> names;
  for (auto &m : methods) {
    fields.push_back(ptr_type);
    names.push_back(m.name);
  }
  if (names.size() > 64)
    internal_error("interface '" + key + "' has more methods than its "
                   "vtable's writes mask can describe");
  iface_vtable_types[key] =
      llvm::StructType::create(context, fields, "saga.vtable." + key);
  iface_method_names[key] = std::move(names);
}

std::string CodeGen::vtable_type_key(const TypePtr &concrete) {
  switch (concrete->kind) {
  case TypeKind::Struct: {
    auto &info = std::get<StructTypeInfo>(concrete->detail);
    return key_for(info.origin_package, info.name);
  }
  case TypeKind::Enum: {
    auto &info = std::get<EnumTypeInfo>(concrete->detail);
    return key_for(info.origin_package, info.name);
  }
  case TypeKind::Alias: {
    auto &info = std::get<AliasTypeInfo>(concrete->detail);
    return key_for(info.origin_package, info.name);
  }
  default:
    return type_to_string(concrete);
  }
}

// A value receiver cannot be written through, so only a struct's methods set
// bits in the writes mask.
llvm::GlobalVariable *
CodeGen::get_or_create_vtable(const TypePtr &concrete,
                              const TypePtr &iface_type) {
  if (!concrete || !iface_type || iface_type->kind != TypeKind::Interface)
    return nullptr;

  auto &iinfo = std::get<InterfaceTypeInfo>(iface_type->detail);
  std::string type_key = vtable_type_key(concrete);
  std::string iface_key = key_for(iinfo.origin_package, iinfo.name);
  std::string vtable_cache_key = type_key + "::" + iface_key;
  if (auto it = vtable_globals.find(vtable_cache_key);
      it != vtable_globals.end())
    return it->second;
  auto vt_it = iface_vtable_types.find(iface_key);
  if (vt_it == iface_vtable_types.end())
    return nullptr;

  auto &method_names = iface_method_names[iface_key];
  auto shape = unwrap_alias(concrete);
  auto *sinfo = shape->kind == TypeKind::Struct
                    ? &std::get<StructTypeInfo>(shape->detail)
                    : nullptr;
  std::vector<llvm::Constant *> entries{
      llvm::ConstantInt::get(i64_type, size_of(llvm_type(concrete))),
      elem_ops_for(concrete),
      llvm::ConstantInt::get(i64_type,
                             sinfo ? writes_mask(*sinfo, method_names) : 0)};
  for (size_t mi = 0; mi < method_names.size(); ++mi) {
    auto *sig = func_info(iinfo.methods[mi]);
    entries.push_back(
        sinfo ? vtable_method(*sinfo, method_names[mi], sig)
              : value_receiver_thunk(concrete, type_key + "." + iface_key,
                                     method_names[mi], sig));
  }

  auto *vtable_global = new llvm::GlobalVariable(
      *module, vt_it->second, true, llvm::GlobalValue::PrivateLinkage,
      llvm::ConstantStruct::get(vt_it->second, entries),
      "saga.vtable." + type_key + "." + iface_key);
  vtable_globals[vtable_cache_key] = vtable_global;
  return vtable_global;
}

// A symbol this package cannot see (a struct from package A satisfying an
// interface from B, boxed in C) is declared against the interface's signature
// for the linker to resolve.
llvm::Constant *CodeGen::vtable_method(const StructTypeInfo &sinfo,
                                       const std::string &method,
                                       const FuncTypeInfo *iface_sig) {
  std::string origin =
      sinfo.origin_package.empty() ? package_name : sinfo.origin_package;
  std::string link_name = mangle(origin, sinfo.name + "__" + method);
  if (auto *fn = module->getFunction(link_name))
    return fn;
  if (!iface_sig)
    internal_error("interface method '" + method + "' has no signature to "
                   "declare '" + link_name + "' against");
  return forward_declare_method(link_name, *iface_sig);
}

// A method the struct doesn't declare itself is promoted from an embed. One
// found nowhere is taken to write, which costs a copy rather than letting a
// shared box see the write.
uint64_t CodeGen::writes_mask(const StructTypeInfo &sinfo,
                              const std::vector<std::string> &methods) {
  uint64_t mask = 0;
  for (size_t i = 0; i < methods.size(); ++i)
    if (method_writes(sinfo, methods[i]).value_or(true))
      mask |= uint64_t{1} << i;
  return mask;
}

std::optional<bool> CodeGen::method_writes(const StructTypeInfo &sinfo,
                                           const std::string &method) {
  for (auto &m : sinfo.methods)
    if (m.name == method)
      return m.mutates_receiver;
  for (auto &embed : sinfo.embeds) {
    auto shape = unwrap_alias(embed);
    if (shape && shape->kind == TypeKind::Struct)
      if (auto writes = method_writes(
              std::get<StructTypeInfo>(shape->detail), method))
        return writes;
  }
  return std::nullopt;
}

// The box takes over the value's references: whoever boxes a borrowed value
// retains it first, as for any other binding.
llvm::Value *CodeGen::emit_interface_box(llvm::Value *concrete_val,
                                         const TypePtr &concrete_type,
                                         const TypePtr &iface_type) {
  auto *vtable = get_or_create_vtable(concrete_type, iface_type);
  if (!vtable)
    internal_error("no vtable for '" + type_to_string(concrete_type) +
                   "' as '" + type_to_string(iface_type) + "'");
  auto *box = builder.CreateCall(module->getFunction("saga_box_new"), {vtable},
                                 "box");
  store_into_slot(box_value(box), llvm_type(concrete_type), concrete_val);
  return box;
}

llvm::Value *CodeGen::box_value(llvm::Value *box) {
  return builder.CreateConstInBoundsGEP1_64(llvm::Type::getInt8Ty(context), box,
                                            kBoxValueOffset, "box.value");
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
  auto *fi = method_signature(iface_info.methods, method);
  if (mn_it == iface_method_names.end() ||
      vt_it == iface_vtable_types.end() || !fi)
    return nullptr;
  auto pos = std::find(mn_it->second.begin(), mn_it->second.end(), method);
  if (pos == mn_it->second.end())
    return nullptr;
  unsigned method_idx = static_cast<unsigned>(pos - mn_it->second.begin());

  auto [box, temporary] =
      interface_receiver(*sel.object, obj, obj_sem, method_idx);
  auto *ptr_type = llvm::PointerType::getUnqual(context);
  auto *vtable = builder.CreateLoad(
      ptr_type,
      builder.CreateConstInBoundsGEP1_64(llvm::Type::getInt8Ty(context), box,
                                         8 * kBoxVtableField, "box.vtable.ptr"),
      "vtable");
  auto *fn_ptr = builder.CreateLoad(
      ptr_type,
      builder.CreateStructGEP(vt_it->second, vtable,
                              kVtablePrefix + method_idx, "vfn.ptr"),
      "vfn");

  // The vtable holds the concrete type's method itself, so the call takes
  // the ABI that method was declared with: the boxed value as its receiver.
  auto *result = emit_call(fn_ptr, lower_signature(*fi, ptr_type),
                           box_value(box), emit_arguments(node, fi, true));
  if (temporary)
    emit_release(box, obj_sem);
  return result;
}

// A call through a method that writes gets a box no other name shares, so
// the write is the binding's own. A binding keeps that box; a temporary's is
// released once the call is done.
std::pair<llvm::Value *, bool>
CodeGen::interface_receiver(const Node &object, llvm::Value *obj,
                            const TypePtr &iface_sem, unsigned method) {
  auto *unique_for = module->getFunction("saga_box_unique_for");
  auto *index = llvm::ConstantInt::get(i64_type, method);
  auto [holder, holder_ll] = assign_target_address(object);
  if (!holder || !holder_ll->isPointerTy())
    return {builder.CreateCall(unique_for, {obj, index}, "box.call"), true};

  auto *cur = builder.CreateLoad(holder_ll, holder, "box.cur");
  auto *box = builder.CreateCall(unique_for, {cur, index}, "box.call");
  builder.CreateStore(box, holder);
  emit_release(cur, iface_sem);
  return {box, false};
}

} // namespace saga

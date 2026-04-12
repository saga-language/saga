// Copyright 2026 Rob Thornton
// SPDX-License-Identifier: MIT

#include "ir/codegen.hpp"

#include <llvm/IR/Constants.h>
#include <llvm/IR/GlobalVariable.h>

namespace mc {

void CodeGen::declare_runtime() {
  auto *ptr_type = llvm::PointerType::getUnqual(context);

  // void saga_intrinsic_print(mc_string* s)
  llvm::Function::Create(
      llvm::FunctionType::get(void_ll_type, {ptr_type}, false),
      llvm::Function::ExternalLinkage, "saga_intrinsic_print", module.get());

  // mc_string* saga_string_concat(mc_string* a, mc_string* b)
  llvm::Function::Create(
      llvm::FunctionType::get(ptr_type, {ptr_type, ptr_type}, false),
      llvm::Function::ExternalLinkage, "saga_string_concat", module.get());

  // int64_t saga_string_compare(mc_string* a, mc_string* b)
  llvm::Function::Create(
      llvm::FunctionType::get(i64_type, {ptr_type, ptr_type}, false),
      llvm::Function::ExternalLinkage, "saga_string_compare", module.get());

  // mc_string* saga_int_to_string(int64_t val)
  llvm::Function::Create(
      llvm::FunctionType::get(ptr_type, {i64_type}, false),
      llvm::Function::ExternalLinkage, "saga_int_to_string", module.get());

  // mc_string* saga_float_to_string(double val)
  llvm::Function::Create(
      llvm::FunctionType::get(ptr_type, {f64_type}, false),
      llvm::Function::ExternalLinkage, "saga_float_to_string", module.get());

  // mc_string* saga_bool_to_string(int64_t val)
  llvm::Function::Create(
      llvm::FunctionType::get(ptr_type, {i64_type}, false),
      llvm::Function::ExternalLinkage, "saga_bool_to_string", module.get());

  // mc_string* saga_string_lower(mc_string* s)
  llvm::Function::Create(
      llvm::FunctionType::get(ptr_type, {ptr_type}, false),
      llvm::Function::ExternalLinkage, "saga_string_lower", module.get());

  // mc_string* saga_string_upper(mc_string* s)
  llvm::Function::Create(
      llvm::FunctionType::get(ptr_type, {ptr_type}, false),
      llvm::Function::ExternalLinkage, "saga_string_upper", module.get());

  // mc_array* saga_string_bytes(mc_string* s)
  llvm::Function::Create(
      llvm::FunctionType::get(ptr_type, {ptr_type}, false),
      llvm::Function::ExternalLinkage, "saga_string_bytes", module.get());

  // i64 saga_string_count(mc_string* s)
  llvm::Function::Create(
      llvm::FunctionType::get(i64_type, {ptr_type}, false),
      llvm::Function::ExternalLinkage, "saga_string_count", module.get());

  // mc_array* saga_string_runes(mc_string* s)
  llvm::Function::Create(
      llvm::FunctionType::get(ptr_type, {ptr_type}, false),
      llvm::Function::ExternalLinkage, "saga_string_runes", module.get());

  // i64 saga_string_to_int(mc_string* s, i64* out)
  llvm::Function::Create(
      llvm::FunctionType::get(i64_type, {ptr_type, ptr_type}, false),
      llvm::Function::ExternalLinkage, "saga_string_to_int", module.get());

  // i64 saga_string_to_float(mc_string* s, double* out)
  llvm::Function::Create(
      llvm::FunctionType::get(i64_type, {ptr_type, ptr_type}, false),
      llvm::Function::ExternalLinkage, "saga_string_to_float", module.get());

  // mc_string* saga_string_format(mc_string* self, mc_string* fmt)
  llvm::Function::Create(
      llvm::FunctionType::get(ptr_type, {ptr_type, ptr_type}, false),
      llvm::Function::ExternalLinkage, "saga_string_format", module.get());

  // mc_string* saga_int_format(i64 val, mc_string* fmt)
  llvm::Function::Create(
      llvm::FunctionType::get(ptr_type, {i64_type, ptr_type}, false),
      llvm::Function::ExternalLinkage, "saga_int_format", module.get());

  // mc_string* saga_float_format(double val, mc_string* fmt)
  llvm::Function::Create(
      llvm::FunctionType::get(ptr_type, {f64_type, ptr_type}, false),
      llvm::Function::ExternalLinkage, "saga_float_format", module.get());

  // mc_array* saga_array_new(i64 elem_size, i64 initial_cap)
  llvm::Function::Create(
      llvm::FunctionType::get(ptr_type, {i64_type, i64_type}, false),
      llvm::Function::ExternalLinkage, "saga_array_new", module.get());

  // void saga_array_push(mc_array* arr, void* elem)
  llvm::Function::Create(
      llvm::FunctionType::get(void_ll_type, {ptr_type, ptr_type}, false),
      llvm::Function::ExternalLinkage, "saga_array_push", module.get());

  // void* saga_array_at(mc_array* arr, i64 index)
  llvm::Function::Create(
      llvm::FunctionType::get(ptr_type, {ptr_type, i64_type}, false),
      llvm::Function::ExternalLinkage, "saga_array_at", module.get());

  // i64 saga_array_size(mc_array* arr)
  llvm::Function::Create(
      llvm::FunctionType::get(i64_type, {ptr_type}, false),
      llvm::Function::ExternalLinkage, "saga_array_size", module.get());

  // i64 saga_array_find(mc_array* arr, void* elem, i64* out)
  llvm::Function::Create(
      llvm::FunctionType::get(i64_type, {ptr_type, ptr_type, ptr_type}, false),
      llvm::Function::ExternalLinkage, "saga_array_find", module.get());

  // void saga_array_insert(mc_array* arr, void* elem, i64 index)
  llvm::Function::Create(
      llvm::FunctionType::get(void_ll_type, {ptr_type, ptr_type, i64_type}, false),
      llvm::Function::ExternalLinkage, "saga_array_insert", module.get());

  // void* saga_array_pop(mc_array* arr)
  llvm::Function::Create(
      llvm::FunctionType::get(ptr_type, {ptr_type}, false),
      llvm::Function::ExternalLinkage, "saga_array_pop", module.get());

  // void saga_array_set(mc_array* arr, i64 index, void* elem)
  llvm::Function::Create(
      llvm::FunctionType::get(void_ll_type, {ptr_type, i64_type, ptr_type}, false),
      llvm::Function::ExternalLinkage, "saga_array_set", module.get());

  // void saga_retain_string(mc_string* s)
  llvm::Function::Create(
      llvm::FunctionType::get(void_ll_type, {ptr_type}, false),
      llvm::Function::ExternalLinkage, "saga_retain_string", module.get());

  // void saga_release_string(mc_string* s)
  llvm::Function::Create(
      llvm::FunctionType::get(void_ll_type, {ptr_type}, false),
      llvm::Function::ExternalLinkage, "saga_release_string", module.get());

  // void saga_retain_array(mc_array* arr)
  llvm::Function::Create(
      llvm::FunctionType::get(void_ll_type, {ptr_type}, false),
      llvm::Function::ExternalLinkage, "saga_retain_array", module.get());

  // void saga_release_array(mc_array* arr)
  llvm::Function::Create(
      llvm::FunctionType::get(void_ll_type, {ptr_type}, false),
      llvm::Function::ExternalLinkage, "saga_release_array", module.get());

  // mc_map* saga_map_new(i64 key_size, i64 val_size, i64 is_string_key)
  llvm::Function::Create(
      llvm::FunctionType::get(ptr_type, {i64_type, i64_type, i64_type}, false),
      llvm::Function::ExternalLinkage, "saga_map_new", module.get());

  // void saga_map_set(mc_map* m, void* key, void* value)
  llvm::Function::Create(
      llvm::FunctionType::get(void_ll_type, {ptr_type, ptr_type, ptr_type}, false),
      llvm::Function::ExternalLinkage, "saga_map_set", module.get());

  // void* saga_map_get(mc_map* m, void* key)
  llvm::Function::Create(
      llvm::FunctionType::get(ptr_type, {ptr_type, ptr_type}, false),
      llvm::Function::ExternalLinkage, "saga_map_get", module.get());

  // i64 saga_map_has(mc_map* m, void* key)
  llvm::Function::Create(
      llvm::FunctionType::get(i64_type, {ptr_type, ptr_type}, false),
      llvm::Function::ExternalLinkage, "saga_map_has", module.get());

  // void saga_map_remove(mc_map* m, void* key)
  llvm::Function::Create(
      llvm::FunctionType::get(void_ll_type, {ptr_type, ptr_type}, false),
      llvm::Function::ExternalLinkage, "saga_map_remove", module.get());

  // i64 saga_map_size(mc_map* m)
  llvm::Function::Create(
      llvm::FunctionType::get(i64_type, {ptr_type}, false),
      llvm::Function::ExternalLinkage, "saga_map_size", module.get());

  // void* saga_map_key_at(mc_map* m, i64 index)
  llvm::Function::Create(
      llvm::FunctionType::get(ptr_type, {ptr_type, i64_type}, false),
      llvm::Function::ExternalLinkage, "saga_map_key_at", module.get());

  // void* saga_map_value_at(mc_map* m, i64 index)
  llvm::Function::Create(
      llvm::FunctionType::get(ptr_type, {ptr_type, i64_type}, false),
      llvm::Function::ExternalLinkage, "saga_map_value_at", module.get());

  // mc_array* saga_map_keys(mc_map* m)
  llvm::Function::Create(
      llvm::FunctionType::get(ptr_type, {ptr_type}, false),
      llvm::Function::ExternalLinkage, "saga_map_keys", module.get());

  // void saga_retain_map(mc_map* m)
  llvm::Function::Create(
      llvm::FunctionType::get(void_ll_type, {ptr_type}, false),
      llvm::Function::ExternalLinkage, "saga_retain_map", module.get());

  // void saga_release_map(mc_map* m)
  llvm::Function::Create(
      llvm::FunctionType::get(void_ll_type, {ptr_type}, false),
      llvm::Function::ExternalLinkage, "saga_release_map", module.get());

  // ── Spawn / Actor runtime functions ────────────────────────────────

  // void saga_executor_init(i64 num_workers)
  llvm::Function::Create(
      llvm::FunctionType::get(void_ll_type, {i64_type}, false),
      llvm::Function::ExternalLinkage, "saga_executor_init", module.get());

  // void saga_executor_shutdown()
  llvm::Function::Create(
      llvm::FunctionType::get(void_ll_type, {}, false),
      llvm::Function::ExternalLinkage, "saga_executor_shutdown", module.get());

  // mc_actor* saga_executor_spawn(void(*entry)(mc_actor*), void* closure,
  //                             i64 closure_size, i64 arena_max)
  llvm::Function::Create(
      llvm::FunctionType::get(ptr_type,
                              {ptr_type, ptr_type, i64_type, i64_type}, false),
      llvm::Function::ExternalLinkage, "saga_executor_spawn", module.get());

  // void saga_executor_schedule(mc_actor* actor)
  llvm::Function::Create(
      llvm::FunctionType::get(void_ll_type, {ptr_type}, false),
      llvm::Function::ExternalLinkage, "saga_executor_schedule", module.get());

  // mc_channel* saga_channel_new(i64 elem_size, i64 capacity)
  llvm::Function::Create(
      llvm::FunctionType::get(ptr_type, {i64_type, i64_type}, false),
      llvm::Function::ExternalLinkage, "saga_channel_new", module.get());

  // int saga_channel_recv(mc_channel* ch, void* out_buf)
  llvm::Function::Create(
      llvm::FunctionType::get(llvm::Type::getInt32Ty(context),
                              {ptr_type, ptr_type}, false),
      llvm::Function::ExternalLinkage, "saga_channel_recv", module.get());

  // void saga_channel_close(mc_channel* ch)
  llvm::Function::Create(
      llvm::FunctionType::get(void_ll_type, {ptr_type}, false),
      llvm::Function::ExternalLinkage, "saga_channel_close", module.get());

  // void saga_channel_destroy(mc_channel* ch)
  llvm::Function::Create(
      llvm::FunctionType::get(void_ll_type, {ptr_type}, false),
      llvm::Function::ExternalLinkage, "saga_channel_destroy", module.get());

  // i64 saga_task_alive(mc_actor* a)
  llvm::Function::Create(
      llvm::FunctionType::get(i64_type, {ptr_type}, false),
      llvm::Function::ExternalLinkage, "saga_task_alive", module.get());

  // void saga_task_cancel(mc_actor* a)
  llvm::Function::Create(
      llvm::FunctionType::get(void_ll_type, {ptr_type}, false),
      llvm::Function::ExternalLinkage, "saga_task_cancel", module.get());

  // void saga_task_term(mc_actor* a)
  llvm::Function::Create(
      llvm::FunctionType::get(void_ll_type, {ptr_type}, false),
      llvm::Function::ExternalLinkage, "saga_task_term", module.get());

  // void* saga_task_wait(mc_actor* a, i64* out_status)
  llvm::Function::Create(
      llvm::FunctionType::get(ptr_type, {ptr_type, ptr_type}, false),
      llvm::Function::ExternalLinkage, "saga_task_wait", module.get());

  // void saga_task_drop(mc_actor* a)
  llvm::Function::Create(
      llvm::FunctionType::get(void_ll_type, {ptr_type}, false),
      llvm::Function::ExternalLinkage, "saga_task_drop", module.get());

  // i64 saga_context_cancelled(mc_actor* a)
  llvm::Function::Create(
      llvm::FunctionType::get(i64_type, {ptr_type}, false),
      llvm::Function::ExternalLinkage, "saga_context_cancelled", module.get());

  // void saga_context_exit(mc_actor* a, void* value, i64 size)
  llvm::Function::Create(
      llvm::FunctionType::get(void_ll_type, {ptr_type, ptr_type, i64_type},
                              false),
      llvm::Function::ExternalLinkage, "saga_context_exit", module.get());

  // int saga_context_send(mc_actor* a, void* data)
  llvm::Function::Create(
      llvm::FunctionType::get(llvm::Type::getInt32Ty(context),
                              {ptr_type, ptr_type}, false),
      llvm::Function::ExternalLinkage, "saga_context_send", module.get());

  // void saga_reduction_tick(mc_actor* a)
  llvm::Function::Create(
      llvm::FunctionType::get(void_ll_type, {ptr_type}, false),
      llvm::Function::ExternalLinkage, "saga_reduction_tick", module.get());

  // void saga_actor_yield(mc_actor* a)
  llvm::Function::Create(
      llvm::FunctionType::get(void_ll_type, {ptr_type}, false),
      llvm::Function::ExternalLinkage, "saga_actor_yield", module.get());

  // void saga_actor_trap(mc_actor* a, mc_string* reason)
  llvm::Function::Create(
      llvm::FunctionType::get(void_ll_type, {ptr_type, ptr_type}, false),
      llvm::Function::ExternalLinkage, "saga_actor_trap", module.get());
}
// ===========================================================================
// Vtable generation
// ===========================================================================

llvm::GlobalVariable *CodeGen::get_or_create_vtable(
    const std::string &struct_name, const std::string &iface_name) {
  std::string key = struct_name + "::" + iface_name;
  auto it = vtable_globals.find(key);
  if (it != vtable_globals.end())
    return it->second;

  auto vt_it = iface_vtable_types.find(iface_name);
  if (vt_it == iface_vtable_types.end())
    return nullptr;
  auto *vtable_st = vt_it->second;

  auto &method_names = iface_method_names[iface_name];
  auto &methods = struct_method_links[struct_name];

  // Build the vtable constant.
  std::vector<llvm::Constant *> entries;
  for (auto &iface_method : method_names) {
    // Find the corresponding struct method.
    std::string link_name = mangle(struct_name + "__" + iface_method);
    auto *fn = module->getFunction(link_name);
    if (fn) {
      entries.push_back(fn);
    } else {
      // Method not found — null pointer (shouldn't happen if analyzer passed).
      entries.push_back(
          llvm::ConstantPointerNull::get(
              llvm::PointerType::getUnqual(context)));
    }
  }

  auto *vtable_const = llvm::ConstantStruct::get(vtable_st, entries);
  auto *vtable_global = new llvm::GlobalVariable(
      *module, vtable_st, true, llvm::GlobalValue::PrivateLinkage,
      vtable_const, "mc.vtable." + struct_name + "." + iface_name);

  vtable_globals[key] = vtable_global;
  return vtable_global;
}

// ===========================================================================
// Interface boxing
// ===========================================================================

llvm::Value *CodeGen::emit_interface_box(llvm::Value *concrete_val,
                                          const TypePtr &concrete_type,
                                          const TypePtr &iface_type) {
  if (!concrete_val || !concrete_type || !iface_type)
    return nullptr;
  if (iface_type->kind != TypeKind::Interface)
    return nullptr;


  auto &iface_info = std::get<InterfaceTypeInfo>(iface_type->detail);
  std::string iface_name = iface_info.name;

  // Determine the struct name.
  std::string struct_name;
  if (concrete_type->kind == TypeKind::Struct) {
    struct_name = std::get<StructTypeInfo>(concrete_type->detail).name;
  } else {
    return nullptr; // Only struct boxing supported for now.
  }

  // Get or create the vtable.
  auto *vtable = get_or_create_vtable(struct_name, iface_name);
  if (!vtable)
    return nullptr;

  // Allocate a fat pointer on the stack.
  auto *func = builder.GetInsertBlock()->getParent();
  auto *fat_alloca = create_entry_alloca(func, "iface.box", iface_fat_ptr_type);

  // Store the data pointer (the concrete struct pointer).
  auto *data_gep = builder.CreateStructGEP(iface_fat_ptr_type, fat_alloca, 0, "iface.data");
  builder.CreateStore(concrete_val, data_gep);

  // Store the vtable pointer.
  auto *vtable_gep = builder.CreateStructGEP(iface_fat_ptr_type, fat_alloca, 1, "iface.vtable");
  builder.CreateStore(vtable, vtable_gep);

  return fat_alloca;
}
// ===========================================================================
// Union helpers
// ===========================================================================

llvm::StructType *CodeGen::get_union_llvm_type(const TypePtr &union_sem) {
  if (!union_sem || union_sem->kind != TypeKind::Union)
    return nullptr;

  std::string key = type_to_string(union_sem);
  auto it = union_llvm_types.find(key);
  if (it != union_llvm_types.end())
    return it->second;

  uint64_t payload = union_payload_size(union_sem);
  // { i8 tag, [payload x i8] }
  auto *payload_ty = llvm::ArrayType::get(
      llvm::Type::getInt8Ty(context), payload);
  auto *st = llvm::StructType::create(
      context,
      {llvm::Type::getInt8Ty(context), payload_ty},
      "mc.union." + key);
  union_llvm_types[key] = st;
  return st;
}

uint64_t CodeGen::union_payload_size(const TypePtr &union_sem) {
  if (!union_sem || union_sem->kind != TypeKind::Union)
    return 8;
  auto &info = std::get<UnionTypeInfo>(union_sem->detail);
  uint64_t max_size = 0;
  auto &dl = module->getDataLayout();
  for (auto &alt : info.alternatives) {
    auto *ll = llvm_type(alt);
    if (ll->isVoidTy())
      continue;
    uint64_t sz = dl.getTypeAllocSize(ll);
    if (sz > max_size)
      max_size = sz;
  }
  return max_size > 0 ? max_size : 8;
}

int CodeGen::union_tag_for_type(const TypePtr &alt_type,
                                 const TypePtr &union_type) {
  if (!union_type || union_type->kind != TypeKind::Union)
    return -1;
  auto &info = std::get<UnionTypeInfo>(union_type->detail);
  for (size_t i = 0; i < info.alternatives.size(); ++i) {
    if (types_equal(info.alternatives[i], alt_type))
      return static_cast<int>(i);
    // Also check interface satisfaction (e.g. Missing satisfies Error).
    if (info.alternatives[i]->kind == TypeKind::Interface &&
        is_assignable_to(alt_type, info.alternatives[i]))
      return static_cast<int>(i);
  }
  return -1;
}

llvm::Value *CodeGen::emit_union_wrap(llvm::Value *val,
                                       const TypePtr &val_type,
                                       const TypePtr &union_type) {
  if (!val || !union_type || union_type->kind != TypeKind::Union)
    return val;

  auto *union_st = get_union_llvm_type(union_type);
  if (!union_st)
    return val;

  int tag = union_tag_for_type(val_type, union_type);
  if (tag < 0)
    return val; // Type not in union — shouldn't happen after analysis.

  auto *func = builder.GetInsertBlock()->getParent();
  auto *alloca = create_entry_alloca(func, "union.tmp", union_st);

  // Zero-initialize the whole struct so padding bytes are clean.
  builder.CreateStore(llvm::Constant::getNullValue(union_st), alloca);

  // Store the tag.
  auto *tag_gep = builder.CreateStructGEP(union_st, alloca, 0, "union.tag");
  builder.CreateStore(
      llvm::ConstantInt::get(llvm::Type::getInt8Ty(context), tag), tag_gep);

  // Store the value into the payload.
  if (!val->getType()->isVoidTy()) {
    auto *payload_gep = builder.CreateStructGEP(union_st, alloca, 1,
                                                 "union.payload");
    auto *cast = builder.CreateBitOrPointerCast(
        payload_gep, llvm::PointerType::getUnqual(context), "union.pcast");
    builder.CreateStore(val, cast);
  }

  return alloca;
}

llvm::Value *CodeGen::emit_union_extract(llvm::Value *union_ptr,
                                          const TypePtr &alt_type,
                                          const TypePtr &union_type) {
  if (!union_ptr || !union_type || union_type->kind != TypeKind::Union)
    return nullptr;

  auto *union_st = get_union_llvm_type(union_type);
  if (!union_st)
    return nullptr;

  auto *ll_alt = llvm_type(alt_type);
  if (ll_alt->isVoidTy())
    return nullptr;

  auto *payload_gep = builder.CreateStructGEP(union_st, union_ptr, 1,
                                               "union.payload");
  auto *cast = builder.CreateBitOrPointerCast(
      payload_gep, llvm::PointerType::getUnqual(context), "union.ecast");
  return builder.CreateLoad(ll_alt, cast, "union.val");
}

bool CodeGen::is_impure_union(const TypePtr &t) const {
  if (!t || t->kind != TypeKind::Union)
    return false;
  auto &info = std::get<UnionTypeInfo>(t->detail);
  for (auto &alt : info.alternatives) {
    if (alt->kind == TypeKind::Interface) {
      auto &iface = std::get<InterfaceTypeInfo>(alt->detail);
      if (iface.name == "Error")
        return true;
    }
  }
  return false;
}

TypePtr CodeGen::strip_error_from_union(const TypePtr &t) const {
  if (!t || t->kind != TypeKind::Union)
    return t;
  auto &info = std::get<UnionTypeInfo>(t->detail);
  std::vector<TypePtr> purified;
  for (auto &alt : info.alternatives) {
    if (alt->kind == TypeKind::Interface) {
      auto &iface = std::get<InterfaceTypeInfo>(alt->detail);
      if (iface.name == "Error")
        continue;
    }
    purified.push_back(alt);
  }
  if (purified.empty())
    return nullptr;
  if (purified.size() == 1)
    return purified[0];
  return make_union_type(std::move(purified));
}

// ===========================================================================
// Reference counting
// ===========================================================================

void CodeGen::track_managed(const std::string &name, const TypePtr &sem) {
  if (!sem) return;
  if (sem->kind == TypeKind::String)
    managed_locals.push_back({name, ManagedKind::String});
  else if (sem->kind == TypeKind::Array)
    managed_locals.push_back({name, ManagedKind::Array});
  else if (sem->kind == TypeKind::Map)
    managed_locals.push_back({name, ManagedKind::Map});
  else if (sem->kind == TypeKind::Struct) {
    auto &info = std::get<StructTypeInfo>(sem->detail);
    if (info.name == "Task") {
      managed_locals.push_back({name, ManagedKind::Task});
    } else {
      // Check if the struct implements the Closer protocol (has Close() Void).
      for (auto &m : info.methods) {
        if (m.name == "Close" && m.signature &&
            m.signature->kind == TypeKind::Func) {
          auto &fi = std::get<FuncTypeInfo>(m.signature->detail);
          if (fi.params.empty()) {
            managed_locals.push_back({name, ManagedKind::Closeable});
            break;
          }
        }
      }
    }
  }
}

void CodeGen::emit_retain(llvm::Value *val, const TypePtr &sem) {
  if (!val || !sem) return;
  if (sem->kind == TypeKind::String) {
    builder.CreateCall(module->getFunction("saga_retain_string"), {val});
  } else if (sem->kind == TypeKind::Array) {
    builder.CreateCall(module->getFunction("saga_retain_array"), {val});
  } else if (sem->kind == TypeKind::Map) {
    builder.CreateCall(module->getFunction("saga_retain_map"), {val});
  }
}

void CodeGen::emit_release(llvm::Value *val, const TypePtr &sem) {
  if (!val || !sem) return;
  if (sem->kind == TypeKind::String) {
    builder.CreateCall(module->getFunction("saga_release_string"), {val});
  } else if (sem->kind == TypeKind::Array) {
    builder.CreateCall(module->getFunction("saga_release_array"), {val});
  } else if (sem->kind == TypeKind::Map) {
    builder.CreateCall(module->getFunction("saga_release_map"), {val});
  }
}

void CodeGen::emit_release_locals() {
  for (auto &ml : managed_locals) {
    auto it = locals.find(ml.name);
    if (it == locals.end()) continue;
    auto *alloca = it->second;
    auto *val = builder.CreateLoad(alloca->getAllocatedType(), alloca);
    if (ml.kind == ManagedKind::String)
      builder.CreateCall(module->getFunction("saga_release_string"), {val});
    else if (ml.kind == ManagedKind::Array)
      builder.CreateCall(module->getFunction("saga_release_array"), {val});
    else if (ml.kind == ManagedKind::Map)
      builder.CreateCall(module->getFunction("saga_release_map"), {val});
    else if (ml.kind == ManagedKind::Task)
      builder.CreateCall(module->getFunction("saga_task_drop"), {val});
    else if (ml.kind == ManagedKind::Closeable) {
      // Call the struct's Close() method.  The alloca is the self ptr.
      auto *alloca = it->second;

      // Look up the struct's semantic type to resolve the Close() link name.
      // We scan the analyzer's node_types to find the type, but it's
      // simpler to check struct_method_links directly.
      // Find the struct name from the alloca's allocated type.
      std::string struct_name;
      for (auto &[sname, st] : struct_types) {
        if (st == alloca->getAllocatedType()) {
          struct_name = sname;
          break;
        }
      }
      if (!struct_name.empty()) {
        // Resolve the Close() link name.
        std::string close_link;
        auto ml_it = struct_method_links.find(struct_name);
        if (ml_it != struct_method_links.end()) {
          for (auto &[lname, mname] : ml_it->second) {
            if (mname == "Close") {
              close_link = lname;
              break;
            }
          }
        }
        if (close_link.empty())
          close_link = mangle(struct_name + "__Close");

        auto *close_fn = module->getFunction(close_link);
        if (!close_fn) {
          // Forward-declare: fn Close(self *Struct) Void
          auto *ptr_type = llvm::PointerType::getUnqual(context);
          auto *fn_type =
              llvm::FunctionType::get(void_ll_type, {ptr_type}, false);
          close_fn = llvm::Function::Create(
              fn_type, llvm::Function::ExternalLinkage,
              close_link, module.get());
        }
        if (close_fn)
          builder.CreateCall(close_fn, {alloca});
      }
    }
  }
}

} // namespace mc

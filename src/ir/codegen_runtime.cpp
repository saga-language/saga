// Copyright 2026 Rob Thornton
// SPDX-License-Identifier: MIT

#include "ir/codegen.hpp"

#include <llvm/IR/Constants.h>
#include <llvm/IR/GlobalVariable.h>

namespace saga {

void CodeGen::declare_runtime() {
  auto *ptr_type = llvm::PointerType::getUnqual(context);

  // void saga_intrinsic_print(saga_runtime_string* s)
  llvm::Function::Create(
      llvm::FunctionType::get(void_ll_type, {ptr_type}, false),
      llvm::Function::ExternalLinkage, "saga_intrinsic_print", module.get());

  // saga_runtime_string* saga_string_concat(saga_runtime_string* a, saga_runtime_string* b)
  llvm::Function::Create(
      llvm::FunctionType::get(ptr_type, {ptr_type, ptr_type}, false),
      llvm::Function::ExternalLinkage, "saga_string_concat", module.get());

  // int64_t saga_string_compare(saga_runtime_string* a, saga_runtime_string* b)
  llvm::Function::Create(
      llvm::FunctionType::get(i64_type, {ptr_type, ptr_type}, false),
      llvm::Function::ExternalLinkage, "saga_string_compare", module.get());

  // saga_runtime_string* saga_int_to_string(int64_t val)
  llvm::Function::Create(
      llvm::FunctionType::get(ptr_type, {i64_type}, false),
      llvm::Function::ExternalLinkage, "saga_int_to_string", module.get());

  // saga_runtime_string* saga_float_to_string(double val)
  llvm::Function::Create(
      llvm::FunctionType::get(ptr_type, {f64_type}, false),
      llvm::Function::ExternalLinkage, "saga_float_to_string", module.get());

  // saga_runtime_string* saga_bool_to_string(int64_t val)
  llvm::Function::Create(
      llvm::FunctionType::get(ptr_type, {i64_type}, false),
      llvm::Function::ExternalLinkage, "saga_bool_to_string", module.get());

  // int64_t saga_int_hash(int64_t v)
  llvm::Function::Create(
      llvm::FunctionType::get(i64_type, {i64_type}, false),
      llvm::Function::ExternalLinkage, "saga_int_hash", module.get());

  // int64_t saga_string_hash(saga_runtime_string* s)
  llvm::Function::Create(
      llvm::FunctionType::get(i64_type, {ptr_type}, false),
      llvm::Function::ExternalLinkage, "saga_string_hash", module.get());

  // int64_t saga_bool_hash(int64_t v)
  llvm::Function::Create(
      llvm::FunctionType::get(i64_type, {i64_type}, false),
      llvm::Function::ExternalLinkage, "saga_bool_hash", module.get());

  // saga_runtime_string* saga_string_lower(saga_runtime_string* s)
  llvm::Function::Create(
      llvm::FunctionType::get(ptr_type, {ptr_type}, false),
      llvm::Function::ExternalLinkage, "saga_string_lower", module.get());

  // saga_runtime_string* saga_string_upper(saga_runtime_string* s)
  llvm::Function::Create(
      llvm::FunctionType::get(ptr_type, {ptr_type}, false),
      llvm::Function::ExternalLinkage, "saga_string_upper", module.get());

  // saga_runtime_string* saga_string_trim(saga_runtime_string* s)
  llvm::Function::Create(
      llvm::FunctionType::get(ptr_type, {ptr_type}, false),
      llvm::Function::ExternalLinkage, "saga_string_trim", module.get());

  // saga_runtime_string* saga_string_capitalize(saga_runtime_string* s)
  llvm::Function::Create(
      llvm::FunctionType::get(ptr_type, {ptr_type}, false),
      llvm::Function::ExternalLinkage, "saga_string_capitalize", module.get());

  // saga_runtime_string* saga_string_title(saga_runtime_string* s)
  llvm::Function::Create(
      llvm::FunctionType::get(ptr_type, {ptr_type}, false),
      llvm::Function::ExternalLinkage, "saga_string_title", module.get());

  // i64 saga_string_has_prefix(saga_runtime_string* s, saga_runtime_string* prefix)
  llvm::Function::Create(
      llvm::FunctionType::get(i64_type, {ptr_type, ptr_type}, false),
      llvm::Function::ExternalLinkage, "saga_string_has_prefix", module.get());

  // i64 saga_string_has_suffix(saga_runtime_string* s, saga_runtime_string* suffix)
  llvm::Function::Create(
      llvm::FunctionType::get(i64_type, {ptr_type, ptr_type}, false),
      llvm::Function::ExternalLinkage, "saga_string_has_suffix", module.get());

  // i64 saga_string_contains(saga_runtime_string* s, saga_runtime_string* needle)
  llvm::Function::Create(
      llvm::FunctionType::get(i64_type, {ptr_type, ptr_type}, false),
      llvm::Function::ExternalLinkage, "saga_string_contains", module.get());

  // saga_runtime_array* saga_string_split(saga_runtime_string* s, saga_runtime_string* sep)
  llvm::Function::Create(
      llvm::FunctionType::get(ptr_type, {ptr_type, ptr_type}, false),
      llvm::Function::ExternalLinkage, "saga_string_split", module.get());

  // saga_runtime_array* saga_string_bytes(saga_runtime_string* s)
  llvm::Function::Create(
      llvm::FunctionType::get(ptr_type, {ptr_type}, false),
      llvm::Function::ExternalLinkage, "saga_string_bytes", module.get());

  // i64 saga_string_count(saga_runtime_string* s)
  llvm::Function::Create(
      llvm::FunctionType::get(i64_type, {ptr_type}, false),
      llvm::Function::ExternalLinkage, "saga_string_count", module.get());

  // saga_runtime_array* saga_string_runes(saga_runtime_string* s)
  llvm::Function::Create(
      llvm::FunctionType::get(ptr_type, {ptr_type}, false),
      llvm::Function::ExternalLinkage, "saga_string_runes", module.get());

  // saga_runtime_string* saga_string_format(saga_runtime_string* self, saga_runtime_string* fmt)
  llvm::Function::Create(
      llvm::FunctionType::get(ptr_type, {ptr_type, ptr_type}, false),
      llvm::Function::ExternalLinkage, "saga_string_format", module.get());

  // saga_runtime_string* saga_int_format(i64 val, saga_runtime_string* fmt)
  llvm::Function::Create(
      llvm::FunctionType::get(ptr_type, {i64_type, ptr_type}, false),
      llvm::Function::ExternalLinkage, "saga_int_format", module.get());

  // saga_runtime_string* saga_float_format(double val, saga_runtime_string* fmt)
  llvm::Function::Create(
      llvm::FunctionType::get(ptr_type, {f64_type, ptr_type}, false),
      llvm::Function::ExternalLinkage, "saga_float_format", module.get());

  // saga_runtime_array* saga_array_new(i64 elem_size, i64 initial_cap)
  llvm::Function::Create(
      llvm::FunctionType::get(ptr_type, {i64_type, i64_type}, false),
      llvm::Function::ExternalLinkage, "saga_array_new", module.get());

  // void saga_array_push(saga_runtime_array* arr, void* elem)
  llvm::Function::Create(
      llvm::FunctionType::get(void_ll_type, {ptr_type, ptr_type}, false),
      llvm::Function::ExternalLinkage, "saga_array_push", module.get());

  // void* saga_array_at(saga_runtime_array* arr, i64 index)
  llvm::Function::Create(
      llvm::FunctionType::get(ptr_type, {ptr_type, i64_type}, false),
      llvm::Function::ExternalLinkage, "saga_array_at", module.get());

  // i64 saga_array_size(saga_runtime_array* arr)
  llvm::Function::Create(
      llvm::FunctionType::get(i64_type, {ptr_type}, false),
      llvm::Function::ExternalLinkage, "saga_array_size", module.get());

  // void saga_array_insert(saga_runtime_array* arr, void* elem, i64 index)
  llvm::Function::Create(
      llvm::FunctionType::get(void_ll_type, {ptr_type, ptr_type, i64_type}, false),
      llvm::Function::ExternalLinkage, "saga_array_insert", module.get());

  // void* saga_array_pop(saga_runtime_array* arr)
  llvm::Function::Create(
      llvm::FunctionType::get(ptr_type, {ptr_type}, false),
      llvm::Function::ExternalLinkage, "saga_array_pop", module.get());

  // void saga_array_set(saga_runtime_array* arr, i64 index, void* elem)
  llvm::Function::Create(
      llvm::FunctionType::get(void_ll_type, {ptr_type, i64_type, ptr_type}, false),
      llvm::Function::ExternalLinkage, "saga_array_set", module.get());

  // i64 saga_array_equals(saga_runtime_array* a, saga_runtime_array* b)
  llvm::Function::Create(
      llvm::FunctionType::get(i64_type, {ptr_type, ptr_type}, false),
      llvm::Function::ExternalLinkage, "saga_array_equals", module.get());

  // saga_runtime_array* saga_array_clone(const saga_runtime_array* src)
  llvm::Function::Create(
      llvm::FunctionType::get(ptr_type, {ptr_type}, false),
      llvm::Function::ExternalLinkage, "saga_array_clone", module.get());

  // saga_runtime_array* saga_range_to_array(i64 low, i64 high)
  llvm::Function::Create(
      llvm::FunctionType::get(ptr_type, {i64_type, i64_type}, false),
      llvm::Function::ExternalLinkage, "saga_range_to_array", module.get());

  // saga_runtime_string* saga_string_at(saga_runtime_string* s, i64 index)
  llvm::Function::Create(
      llvm::FunctionType::get(ptr_type, {ptr_type, i64_type}, false),
      llvm::Function::ExternalLinkage, "saga_string_at", module.get());

  // saga_runtime_string* saga_string_slice(saga_runtime_string* s,
  //                                        i64 low, i64 high)
  llvm::Function::Create(
      llvm::FunctionType::get(ptr_type, {ptr_type, i64_type, i64_type}, false),
      llvm::Function::ExternalLinkage, "saga_string_slice", module.get());

  // void* saga_missing_new(const char* msg, i64 len)
  // Returns a heap-allocated saga_runtime_iface_fat_ptr (data = Missing
  // instance carrying `msg`, vtable = Missing-as-Error vtable).  Callers
  // wrap the returned pointer into a `T | Error` union's err payload so
  // user code can dispatch `err.Message()` through the Error interface.
  llvm::Function::Create(
      llvm::FunctionType::get(ptr_type, {ptr_type, i64_type}, false),
      llvm::Function::ExternalLinkage, "saga_missing_new", module.get());

  // void saga_retain_string(saga_runtime_string* s)
  llvm::Function::Create(
      llvm::FunctionType::get(void_ll_type, {ptr_type}, false),
      llvm::Function::ExternalLinkage, "saga_retain_string", module.get());

  // void saga_release_string(saga_runtime_string* s)
  llvm::Function::Create(
      llvm::FunctionType::get(void_ll_type, {ptr_type}, false),
      llvm::Function::ExternalLinkage, "saga_release_string", module.get());

  // void saga_retain_array(saga_runtime_array* arr)
  llvm::Function::Create(
      llvm::FunctionType::get(void_ll_type, {ptr_type}, false),
      llvm::Function::ExternalLinkage, "saga_retain_array", module.get());

  // void saga_release_array(saga_runtime_array* arr)
  llvm::Function::Create(
      llvm::FunctionType::get(void_ll_type, {ptr_type}, false),
      llvm::Function::ExternalLinkage, "saga_release_array", module.get());

  // saga_runtime_map* saga_map_new(i64 key_size, i64 val_size,
  //                                i64 key_kind, saga_runtime_key_ops* ops)
  llvm::Function::Create(
      llvm::FunctionType::get(
          ptr_type, {i64_type, i64_type, i64_type, ptr_type}, false),
      llvm::Function::ExternalLinkage, "saga_map_new", module.get());

  // void saga_map_set(saga_runtime_map* m, void* key, void* value)
  llvm::Function::Create(
      llvm::FunctionType::get(void_ll_type, {ptr_type, ptr_type, ptr_type}, false),
      llvm::Function::ExternalLinkage, "saga_map_set", module.get());

  // void* saga_map_get(saga_runtime_map* m, void* key)
  llvm::Function::Create(
      llvm::FunctionType::get(ptr_type, {ptr_type, ptr_type}, false),
      llvm::Function::ExternalLinkage, "saga_map_get", module.get());

  // i64 saga_map_has(saga_runtime_map* m, void* key)
  llvm::Function::Create(
      llvm::FunctionType::get(i64_type, {ptr_type, ptr_type}, false),
      llvm::Function::ExternalLinkage, "saga_map_has", module.get());

  // void saga_map_remove(saga_runtime_map* m, void* key)
  llvm::Function::Create(
      llvm::FunctionType::get(void_ll_type, {ptr_type, ptr_type}, false),
      llvm::Function::ExternalLinkage, "saga_map_remove", module.get());

  // i64 saga_map_size(saga_runtime_map* m)
  llvm::Function::Create(
      llvm::FunctionType::get(i64_type, {ptr_type}, false),
      llvm::Function::ExternalLinkage, "saga_map_size", module.get());

  // void* saga_map_key_at(saga_runtime_map* m, i64 index)
  llvm::Function::Create(
      llvm::FunctionType::get(ptr_type, {ptr_type, i64_type}, false),
      llvm::Function::ExternalLinkage, "saga_map_key_at", module.get());

  // void* saga_map_value_at(saga_runtime_map* m, i64 index)
  llvm::Function::Create(
      llvm::FunctionType::get(ptr_type, {ptr_type, i64_type}, false),
      llvm::Function::ExternalLinkage, "saga_map_value_at", module.get());

  // saga_runtime_array* saga_map_keys(saga_runtime_map* m)
  llvm::Function::Create(
      llvm::FunctionType::get(ptr_type, {ptr_type}, false),
      llvm::Function::ExternalLinkage, "saga_map_keys", module.get());

  // i64 saga_map_equals(saga_runtime_map* a, saga_runtime_map* b)
  llvm::Function::Create(
      llvm::FunctionType::get(i64_type, {ptr_type, ptr_type}, false),
      llvm::Function::ExternalLinkage, "saga_map_equals", module.get());

  // void saga_retain_map(saga_runtime_map* m)
  llvm::Function::Create(
      llvm::FunctionType::get(void_ll_type, {ptr_type}, false),
      llvm::Function::ExternalLinkage, "saga_retain_map", module.get());

  // void saga_release_map(saga_runtime_map* m)
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

  // saga_runtime_actor* saga_executor_spawn(void(*entry)(saga_runtime_actor*), void* closure,
  //                             i64 closure_size, i64 arena_max)
  llvm::Function::Create(
      llvm::FunctionType::get(ptr_type,
                              {ptr_type, ptr_type, i64_type, i64_type}, false),
      llvm::Function::ExternalLinkage, "saga_executor_spawn", module.get());

  // void saga_executor_schedule(saga_runtime_actor* actor)
  llvm::Function::Create(
      llvm::FunctionType::get(void_ll_type, {ptr_type}, false),
      llvm::Function::ExternalLinkage, "saga_executor_schedule", module.get());

  // saga_runtime_channel* saga_channel_new(i64 elem_size, i64 capacity)
  llvm::Function::Create(
      llvm::FunctionType::get(ptr_type, {i64_type, i64_type}, false),
      llvm::Function::ExternalLinkage, "saga_channel_new", module.get());

  // int saga_channel_recv(saga_runtime_channel* ch, void* out_buf)
  llvm::Function::Create(
      llvm::FunctionType::get(llvm::Type::getInt32Ty(context),
                              {ptr_type, ptr_type}, false),
      llvm::Function::ExternalLinkage, "saga_channel_recv", module.get());

  // void saga_channel_close(saga_runtime_channel* ch)
  llvm::Function::Create(
      llvm::FunctionType::get(void_ll_type, {ptr_type}, false),
      llvm::Function::ExternalLinkage, "saga_channel_close", module.get());

  // void saga_channel_destroy(saga_runtime_channel* ch)
  llvm::Function::Create(
      llvm::FunctionType::get(void_ll_type, {ptr_type}, false),
      llvm::Function::ExternalLinkage, "saga_channel_destroy", module.get());

  // i64 saga_task_alive(saga_runtime_actor* a)
  llvm::Function::Create(
      llvm::FunctionType::get(i64_type, {ptr_type}, false),
      llvm::Function::ExternalLinkage, "saga_task_alive", module.get());

  // void saga_task_cancel(saga_runtime_actor* a)
  llvm::Function::Create(
      llvm::FunctionType::get(void_ll_type, {ptr_type}, false),
      llvm::Function::ExternalLinkage, "saga_task_cancel", module.get());

  // void saga_task_term(saga_runtime_actor* a)
  llvm::Function::Create(
      llvm::FunctionType::get(void_ll_type, {ptr_type}, false),
      llvm::Function::ExternalLinkage, "saga_task_term", module.get());

  // void* saga_task_wait(saga_runtime_actor* a, i64* out_status)
  llvm::Function::Create(
      llvm::FunctionType::get(ptr_type, {ptr_type, ptr_type}, false),
      llvm::Function::ExternalLinkage, "saga_task_wait", module.get());

  // void saga_task_drop(saga_runtime_actor* a)
  llvm::Function::Create(
      llvm::FunctionType::get(void_ll_type, {ptr_type}, false),
      llvm::Function::ExternalLinkage, "saga_task_drop", module.get());

  // i64 saga_context_cancelled(saga_runtime_actor* a)
  llvm::Function::Create(
      llvm::FunctionType::get(i64_type, {ptr_type}, false),
      llvm::Function::ExternalLinkage, "saga_context_cancelled", module.get());

  // void saga_context_exit(saga_runtime_actor* a, void* value, i64 size)
  llvm::Function::Create(
      llvm::FunctionType::get(void_ll_type, {ptr_type, ptr_type, i64_type},
                              false),
      llvm::Function::ExternalLinkage, "saga_context_exit", module.get());

  // int saga_context_send(saga_runtime_actor* a, void* data)
  llvm::Function::Create(
      llvm::FunctionType::get(llvm::Type::getInt32Ty(context),
                              {ptr_type, ptr_type}, false),
      llvm::Function::ExternalLinkage, "saga_context_send", module.get());

  // void saga_reduction_tick(saga_runtime_actor* a)
  llvm::Function::Create(
      llvm::FunctionType::get(void_ll_type, {ptr_type}, false),
      llvm::Function::ExternalLinkage, "saga_reduction_tick", module.get());

  // void saga_actor_yield(void)
  llvm::Function::Create(
      llvm::FunctionType::get(void_ll_type, {}, false),
      llvm::Function::ExternalLinkage, "saga_actor_yield", module.get());

  // void saga_actor_trap(saga_runtime_string* reason)
  llvm::Function::Create(
      llvm::FunctionType::get(void_ll_type, {ptr_type}, false),
      llvm::Function::ExternalLinkage, "saga_actor_trap", module.get());

  // void* saga_error_from_trap(saga_runtime_actor* a)
  llvm::Function::Create(
      llvm::FunctionType::get(ptr_type, {ptr_type}, false),
      llvm::Function::ExternalLinkage, "saga_error_from_trap", module.get());

  // Seed the Error interface's codegen registration.  Error is a builtin
  // interface with a single method (Message() String); unlike Saga-declared
  // interfaces it has no InterfaceDeclNode to drive declare_interfaces.
  // Use the current package as the key so key_for("", "Error") resolves it.
  std::string error_key = mangle(package_name, "Error");
  if (!iface_vtable_types.count(error_key)) {
    auto *vtable_st = llvm::StructType::create(
        context, {ptr_type}, "saga.vtable." + error_key);
    iface_vtable_types[error_key] = vtable_st;
    iface_method_names[error_key] = {"Message"};
  }
}
// ===========================================================================
// Vtable generation
// ===========================================================================

llvm::GlobalVariable *CodeGen::get_or_create_vtable(const TypePtr &struct_type,
                                                     const TypePtr &iface_type) {
  if (!struct_type || struct_type->kind != TypeKind::Struct) return nullptr;
  if (!iface_type || iface_type->kind != TypeKind::Interface) return nullptr;

  auto &sinfo = std::get<StructTypeInfo>(struct_type->detail);
  auto &iinfo = std::get<InterfaceTypeInfo>(iface_type->detail);

  std::string struct_key = key_for(sinfo.origin_package, sinfo.name);
  std::string iface_key = key_for(iinfo.origin_package, iinfo.name);

  std::string vtable_cache_key = struct_key + "::" + iface_key;
  auto it = vtable_globals.find(vtable_cache_key);
  if (it != vtable_globals.end())
    return it->second;

  auto vt_it = iface_vtable_types.find(iface_key);
  if (vt_it == iface_vtable_types.end())
    return nullptr;
  auto *vtable_st = vt_it->second;

  auto &method_names = iface_method_names[iface_key];
  auto &iinfo_methods = std::get<InterfaceTypeInfo>(iface_type->detail).methods;

  // Build the vtable constant.
  std::string struct_origin =
      sinfo.origin_package.empty() ? package_name : sinfo.origin_package;
  std::vector<llvm::Constant *> entries;
  for (size_t mi = 0; mi < method_names.size(); ++mi) {
    auto &iface_method = method_names[mi];
    std::string link_name =
        mangle(struct_origin, sinfo.name + "__" + iface_method);
    auto *fn = module->getFunction(link_name);
    if (!fn && mi < iinfo_methods.size() && iinfo_methods[mi].signature &&
        iinfo_methods[mi].signature->kind == TypeKind::Func) {
      // Symbol not visible in this importer (e.g. struct from pkg A satisfying
      // iface from pkg B used from pkg C).  Forward-declare against the iface
      // method signature so the linker resolves it.
      auto &fi = std::get<FuncTypeInfo>(iinfo_methods[mi].signature->detail);
      fn = forward_declare_method(link_name, fi);
    }
    if (fn) {
      entries.push_back(fn);
    } else {
      entries.push_back(
          llvm::ConstantPointerNull::get(
              llvm::PointerType::getUnqual(context)));
    }
  }

  auto *vtable_const = llvm::ConstantStruct::get(vtable_st, entries);
  auto *vtable_global = new llvm::GlobalVariable(
      *module, vtable_st, true, llvm::GlobalValue::PrivateLinkage,
      vtable_const, "saga.vtable." + struct_key + "." + iface_key);

  vtable_globals[vtable_cache_key] = vtable_global;
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


  if (concrete_type->kind != TypeKind::Struct)
    return nullptr; // Only struct boxing supported for now.

  // Get or create the vtable using origin-qualified type pointers.
  auto *vtable = get_or_create_vtable(concrete_type, iface_type);
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
      "saga.union." + key);
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

llvm::Value *CodeGen::emit_missing_fat_ptr(const std::string &message) {
  auto *char_array = llvm::ConstantDataArray::getString(
      context, message, /*AddNull=*/false);
  auto *raw_global = new llvm::GlobalVariable(
      *module, char_array->getType(), /*isConstant=*/true,
      llvm::GlobalValue::PrivateLinkage, char_array, ".missing.msg");
  raw_global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
  raw_global->setAlignment(llvm::Align(1));
  auto *data_ptr = llvm::ConstantExpr::getInBoundsGetElementPtr(
      char_array->getType(), raw_global,
      llvm::ArrayRef<llvm::Constant *>{
          llvm::ConstantInt::get(i64_type, 0),
          llvm::ConstantInt::get(i64_type, 0)});
  auto *len = llvm::ConstantInt::get(i64_type,
                                     static_cast<int64_t>(message.size()));
  return builder.CreateCall(module->getFunction("saga_missing_new"),
                            {data_ptr, len}, "missing.fat");
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
    // Concrete types that satisfy Error (e.g. Missing) also make the
    // union impure for or-clause purposes — `String | Missing` reads
    // the same way as `String | Error` to user code.
    if (alt && alt->kind == TypeKind::Struct) {
      auto &sinfo = std::get<StructTypeInfo>(alt->detail);
      if (sinfo.name == "Missing") return true;
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
    if (alt && alt->kind == TypeKind::Interface) {
      auto &iface = std::get<InterfaceTypeInfo>(alt->detail);
      if (iface.name == "Error") continue;
    }
    if (alt && alt->kind == TypeKind::Struct) {
      auto &sinfo = std::get<StructTypeInfo>(alt->detail);
      if (sinfo.name == "Missing") continue;
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
        // struct_name is the origin-qualified key (e.g. "lib__Point").
        // Resolve the Close() link name from struct_method_links.
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
        // Fallback: struct_name is already "pkg__Name", append "__Close".
        if (close_link.empty())
          close_link = struct_name + "__Close";

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

} // namespace saga

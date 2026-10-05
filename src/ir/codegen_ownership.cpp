// Copyright 2026 Rob Thornton
// SPDX-License-Identifier: MIT

#include "ir/codegen.hpp"

namespace saga {

bool is_boxed(const TypePtr &t) {
  return t->kind == TypeKind::Interface || t->kind == TypeKind::Func;
}

bool is_counted(const TypePtr &t) {
  return t && (t->kind == TypeKind::String || t->kind == TypeKind::Array ||
               t->kind == TypeKind::Map || is_boxed(t));
}

// A local owns its value outright: a Task is dropped and a struct with a
// `Close` is closed when its scope ends.
void CodeGen::track_managed(llvm::AllocaInst *slot, const TypePtr &sem) {
  if (!slot || !sem) return;
  auto *info = sem->kind == TypeKind::Struct
                   ? &std::get<StructTypeInfo>(sem->detail)
                   : nullptr;
  if (info && info->name == "Task")
    managed_locals.push_back({slot, ManagedKind::Task, sem});
  else if (info && has_close_method(*info))
    managed_locals.push_back({slot, ManagedKind::Closeable, sem});
  else
    track_reference(slot, sem);
}

// A parameter owns only the reference its caller handed over; the value
// itself, and closing it, stay with the caller.
void CodeGen::track_reference(llvm::AllocaInst *slot, const TypePtr &sem) {
  if (is_counted(sem))
    managed_locals.push_back({slot, ManagedKind::Counted, sem});
  else if (walks_references(sem))
    managed_locals.push_back({slot, ManagedKind::Struct, sem});
}

bool CodeGen::has_close_method(const StructTypeInfo &info) {
  for (auto &m : info.methods) {
    if (m.name != "Close" || !m.signature ||
        m.signature->kind != TypeKind::Func)
      continue;
    if (std::get<FuncTypeInfo>(m.signature->detail).params.empty())
      return true;
  }
  return false;
}

void CodeGen::emit_retain(llvm::Value *val, const TypePtr &sem) {
  if (!val || !sem) return;
  if (sem->kind == TypeKind::String)
    builder.CreateCall(module->getFunction("saga_retain_string"), {val});
  else if (sem->kind == TypeKind::Array)
    builder.CreateCall(module->getFunction("saga_retain_array"), {val});
  else if (sem->kind == TypeKind::Map)
    builder.CreateCall(module->getFunction("saga_retain_map"), {val});
  else if (is_boxed(sem))
    builder.CreateCall(module->getFunction("saga_box_retain"), {val});
  else if (walks_references(sem))
    emit_ownership_walk(val, sem, true);
}

// A write that lands through the binding, rather than through a value the
// caller stores, needs the buffer to itself first — otherwise a second name
// sees the edit. The slot takes the unique collection and drops the reference
// it held, so it still owns exactly one.
llvm::Value *CodeGen::make_binding_unique(const Node &object,
                                          const TypePtr &sem) {
  const char *unique_fn = nullptr;
  if (sem && sem->kind == TypeKind::Array)
    unique_fn = "saga_array_make_unique";
  else if (sem && sem->kind == TypeKind::Map)
    unique_fn = "saga_map_make_unique";

  auto [holder, holder_ll] =
      unique_fn ? assign_target_address(object)
                : std::pair<llvm::Value *, llvm::Type *>{nullptr, nullptr};
  if (!holder || !holder_ll->isPointerTy())
    return emit_expr(object);

  auto *cur = builder.CreateLoad(holder_ll, holder, "cow.cur");
  auto *uniq =
      builder.CreateCall(module->getFunction(unique_fn), {cur}, "cow.uniq");
  builder.CreateStore(uniq, holder);
  emit_release(cur, sem);
  return uniq;
}

void CodeGen::emit_release(llvm::Value *val, const TypePtr &sem) {
  if (!val || !sem) return;
  if (sem->kind == TypeKind::String)
    builder.CreateCall(module->getFunction("saga_release_string"), {val});
  else if (sem->kind == TypeKind::Array)
    builder.CreateCall(module->getFunction("saga_release_array"), {val});
  else if (sem->kind == TypeKind::Map)
    builder.CreateCall(module->getFunction("saga_release_map"), {val});
  else if (is_boxed(sem))
    builder.CreateCall(module->getFunction("saga_box_release"), {val});
  else if (walks_references(sem))
    emit_ownership_walk(val, sem, false);
}

void CodeGen::emit_release_locals() {
  for (auto &ml : managed_locals) {
    if (ml.kind == ManagedKind::Closeable) {
      emit_close_call(ml.slot);
      continue;
    }
    if (ml.kind == ManagedKind::Struct) {
      emit_release(ml.slot, ml.sem);
      continue;
    }
    auto *val = builder.CreateLoad(ml.slot->getAllocatedType(), ml.slot);
    if (ml.kind == ManagedKind::Task)
      builder.CreateCall(module->getFunction("saga_task_drop"), {val});
    else
      emit_release(val, ml.sem);
  }
}

// The struct's own name is the origin-qualified key, so `Close` resolves
// through the same method-link table method calls go through.
std::string CodeGen::close_link_name(llvm::Type *struct_ll) const {
  std::string key;
  for (auto &[sname, st] : struct_types)
    if (st == struct_ll) { key = sname; break; }
  if (key.empty()) return {};

  auto links = struct_method_links.find(key);
  if (links != struct_method_links.end())
    for (auto &[link, method] : links->second)
      if (method == "Close") return link;
  return key + "__Close";
}

void CodeGen::emit_close_call(llvm::AllocaInst *slot) {
  auto link = close_link_name(slot->getAllocatedType());
  if (link.empty()) return;

  auto *close_fn = module->getFunction(link);
  if (!close_fn) {
    auto *fn_type = llvm::FunctionType::get(
        void_ll_type, {llvm::PointerType::getUnqual(context)}, false);
    close_fn = llvm::Function::Create(fn_type, llvm::Function::ExternalLinkage,
                                      link, module.get());
  }
  builder.CreateCall(close_fn, {slot});
}

} // namespace saga

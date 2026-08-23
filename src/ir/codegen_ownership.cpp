// Copyright 2026 Rob Thornton
// SPDX-License-Identifier: MIT

#include "ir/codegen.hpp"

namespace saga {

void CodeGen::track_managed(llvm::AllocaInst *slot, const TypePtr &sem) {
  if (!slot || !sem) return;
  if (sem->kind == TypeKind::String || sem->kind == TypeKind::Array ||
      sem->kind == TypeKind::Map) {
    managed_locals.push_back({slot, ManagedKind::Counted, sem});
    return;
  }
  if (sem->kind != TypeKind::Struct) return;

  auto &info = std::get<StructTypeInfo>(sem->detail);
  if (info.name == "Task")
    managed_locals.push_back({slot, ManagedKind::Task, sem});
  else if (has_close_method(info))
    managed_locals.push_back({slot, ManagedKind::Closeable, sem});
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
}

// A value read out of an existing binding is borrowed: the slot it came from
// still owns it, so a second slot holding it needs a count of its own.
// Everything else — a literal, a call, a copy-on-write method — hands back a
// reference the runtime has already counted for this binding.
bool CodeGen::is_borrowed_expr(const Node &node) {
  if (auto *group = std::get_if<GroupExprNode>(&node.data))
    return group->inner && is_borrowed_expr(*group->inner);
  return std::holds_alternative<IdentifierNode>(node.data) ||
         std::holds_alternative<SelectorNode>(node.data);
}

void CodeGen::retain_if_borrowed(llvm::Value *val, const TypePtr &sem,
                                 const Node &source) {
  if (val && is_borrowed_expr(source))
    emit_retain(val, sem);
}

void CodeGen::emit_release(llvm::Value *val, const TypePtr &sem) {
  if (!val || !sem) return;
  if (sem->kind == TypeKind::String)
    builder.CreateCall(module->getFunction("saga_release_string"), {val});
  else if (sem->kind == TypeKind::Array)
    builder.CreateCall(module->getFunction("saga_release_array"), {val});
  else if (sem->kind == TypeKind::Map)
    builder.CreateCall(module->getFunction("saga_release_map"), {val});
}

void CodeGen::emit_release_locals() {
  for (auto &ml : managed_locals) {
    if (ml.kind == ManagedKind::Closeable) {
      emit_close_call(ml.slot);
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

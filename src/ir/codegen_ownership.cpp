// Copyright 2026 Rob Thornton
// SPDX-License-Identifier: MIT

#include "ir/codegen.hpp"

namespace saga {

namespace {
bool is_counted(const TypePtr &t) {
  return t && (t->kind == TypeKind::String || t->kind == TypeKind::Array ||
               t->kind == TypeKind::Map);
}
} // namespace

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
  else if (owns_managed_fields(sem))
    managed_locals.push_back({slot, ManagedKind::Struct, sem});
}

// An error box escapes through a union and is never freed, so walking one
// would release a message the box still points at.
bool CodeGen::owns_managed_fields(const TypePtr &sem) {
  auto s = unwrap_alias(sem);
  if (!s || s->kind != TypeKind::Struct) return false;

  auto &info = std::get<StructTypeInfo>(s->detail);
  if (info.is_error) return false;
  for (auto &f : info.fields)
    if (is_counted(unwrap_alias(f.type)) || owns_managed_fields(f.type))
      return true;
  for (auto &e : info.embeds)
    if (owns_managed_fields(e)) return true;
  return false;
}

// A struct crosses as an SSA value where one slot is copied into another, and
// the walk needs an address. The field pointers it counts are the same either
// way, so a spilled copy answers as well as the original slot.
void CodeGen::emit_ownership_walk(llvm::Value *val, const TypePtr &sem,
                                  bool retain) {
  auto *walker = struct_ownership_fn(sem, retain);
  if (!walker) return;
  auto *addr = val->getType()->isPointerTy()
                   ? val
                   : spill_aggregate(val, "own.tmp");
  builder.CreateCall(walker, {addr});
}

llvm::Function *CodeGen::struct_ownership_fn(const TypePtr &sem, bool retain) {
  auto &info = std::get<StructTypeInfo>(unwrap_alias(sem)->detail);
  std::string key = struct_cache_key(info);
  std::string name = key + (retain ? "__retain_fields" : "__release_fields");
  if (auto *existing = module->getFunction(name)) return existing;

  auto st_it = struct_types.find(key);
  if (st_it == struct_types.end()) return nullptr;

  auto *fn = llvm::Function::Create(
      llvm::FunctionType::get(void_ll_type,
                              {llvm::PointerType::getUnqual(context)}, false),
      llvm::Function::InternalLinkage, name, module.get());

  auto saved = builder.saveIP();
  builder.SetInsertPoint(llvm::BasicBlock::Create(context, "entry", fn));
  emit_slot_walk(st_it->second, unwrap_alias(sem), fn->getArg(0), retain);
  builder.CreateRetVoid();
  builder.restoreIP(saved);
  return fn;
}

// Own fields occupy the leading slots and embeds the trailing ones, the layout
// `struct_field_gep` addresses by name.
void CodeGen::emit_slot_walk(llvm::StructType *st, const TypePtr &sem,
                             llvm::Value *self, bool retain) {
  auto &info = std::get<StructTypeInfo>(sem->detail);
  unsigned idx = 0;
  for (auto &f : info.fields)
    emit_slot_ownership(st, self, idx++, f.type, retain);
  for (auto &e : info.embeds)
    emit_slot_ownership(st, self, idx++, e, retain);
}

void CodeGen::emit_slot_ownership(llvm::StructType *st, llvm::Value *self,
                                  unsigned idx, const TypePtr &slot,
                                  bool retain) {
  if (idx >= st->getNumElements()) return;
  auto sem = unwrap_alias(slot);
  if (!is_counted(sem) && !owns_managed_fields(sem)) return;

  auto *gep = builder.CreateStructGEP(st, self, idx);
  if (!is_counted(sem)) {
    if (auto *walker = struct_ownership_fn(sem, retain))
      builder.CreateCall(walker, {gep});
    return;
  }

  auto *val = builder.CreateLoad(llvm::PointerType::getUnqual(context), gep);
  if (retain)
    emit_retain(val, sem);
  else
    emit_release(val, sem);
}

// The slot owns what it holds, so overwriting it drops that reference first.
void CodeGen::release_slot(llvm::Value *addr, llvm::Type *slot_ll,
                           const TypePtr &sem) {
  auto s = unwrap_alias(sem);
  if (owns_managed_fields(s))
    emit_release(addr, s);
  else if (is_counted(s))
    emit_release(builder.CreateLoad(slot_ll, addr), s);
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
  else if (owns_managed_fields(sem))
    emit_ownership_walk(val, sem, true);
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
  else if (owns_managed_fields(sem))
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

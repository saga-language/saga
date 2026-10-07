// Copyright 2026 Rob Thornton
// SPDX-License-Identifier: MIT

// The generated walks that retain or release the references a struct or a
// union holds in its own bytes: a struct's fields, and the payload of the
// alternative a union's tag names. Each takes the value's address, so the same
// walk serves a local, a field, a parameter and a collection slot.

#include "ir/codegen.hpp"

namespace saga {

// An error box escapes through a union and is never freed, so walking one
// would release a message the box still points at. A self-containing union
// alternative is a heap copy nothing frees yet, so it is not walked either.
bool CodeGen::walks_references(const TypePtr &sem) {
  auto s = unwrap_alias(sem);
  if (!s)
    return false;
  if (s->kind == TypeKind::Union) {
    for (auto &alt : std::get<UnionTypeInfo>(s->detail).alternatives)
      if (!union_alt_is_boxed(alt) && holds_references(alt))
        return true;
    return false;
  }
  if (s->kind != TypeKind::Struct)
    return false;
  auto &info = std::get<StructTypeInfo>(s->detail);
  if (info.is_error)
    return false;
  for (auto &f : info.fields)
    if (holds_references(f.type))
      return true;
  for (auto &e : info.embeds)
    if (walks_references(e))
      return true;
  return false;
}

bool CodeGen::holds_references(const TypePtr &sem) {
  return is_counted(unwrap_alias(sem)) || walks_references(sem);
}

// A struct or union crosses as an SSA value where one slot is copied into
// another, and the walk needs an address. The references it counts are the
// same either way, so a spilled copy answers as well as the original slot.
void CodeGen::emit_ownership_walk(llvm::Value *val, const TypePtr &sem,
                                  bool retain) {
  auto *walker = ownership_fn(sem, retain);
  if (!walker) return;
  auto *addr = val->getType()->isPointerTy()
                   ? val
                   : spill_aggregate(val, "own.tmp");
  builder.CreateCall(walker, {addr});
}

llvm::Function *CodeGen::ownership_fn(const TypePtr &sem, bool retain) {
  auto s = unwrap_alias(sem);
  return s->kind == TypeKind::Union ? union_walk_fn(s, retain)
                                    : struct_walk_fn(s, retain);
}

llvm::Function *CodeGen::declare_walk_fn(const std::string &name) {
  return llvm::Function::Create(
      llvm::FunctionType::get(void_ll_type,
                              {llvm::PointerType::getUnqual(context)}, false),
      llvm::Function::InternalLinkage, name, module.get());
}

// A temporary's slot is null on a path that never made its value, and walking
// nothing is a no-op, as releasing a null reference is.
void CodeGen::open_walk(llvm::Function *fn) {
  auto *entry = llvm::BasicBlock::Create(context, "entry", fn);
  auto *none = llvm::BasicBlock::Create(context, "none", fn);
  auto *walk = llvm::BasicBlock::Create(context, "walk", fn);
  builder.SetInsertPoint(entry);
  builder.CreateCondBr(builder.CreateIsNull(fn->getArg(0)), none, walk);
  builder.SetInsertPoint(none);
  builder.CreateRetVoid();
  builder.SetInsertPoint(walk);
}

llvm::Function *CodeGen::struct_walk_fn(const TypePtr &sem, bool retain) {
  auto &info = std::get<StructTypeInfo>(sem->detail);
  std::string key = struct_cache_key(info);
  std::string name = key + (retain ? "__retain_fields" : "__release_fields");
  if (auto *existing = module->getFunction(name)) return existing;

  auto st_it = struct_types.find(key);
  if (st_it == struct_types.end()) return nullptr;

  auto *fn = declare_walk_fn(name);
  auto saved = builder.saveIP();
  open_walk(fn);
  emit_slot_walk(st_it->second, sem, fn->getArg(0), retain);
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
    walk_field(st, self, idx++, f.type, retain);
  for (auto &e : info.embeds)
    walk_field(st, self, idx++, e, retain);
}

void CodeGen::walk_field(llvm::StructType *st, llvm::Value *self, unsigned idx,
                         const TypePtr &slot, bool retain) {
  if (idx < st->getNumElements() && holds_references(slot))
    walk_slot(builder.CreateStructGEP(st, self, idx), slot, retain);
}

llvm::Function *CodeGen::union_walk_fn(const TypePtr &sem, bool retain) {
  auto *union_st = get_union_llvm_type(sem);
  std::string name =
      union_st->getName().str() + (retain ? "__retain" : "__release");
  if (auto *existing = module->getFunction(name)) return existing;

  auto *fn = declare_walk_fn(name);
  auto saved = builder.saveIP();
  open_walk(fn);
  auto *self = fn->getArg(0);
  auto *tag = builder.CreateLoad(llvm::Type::getInt8Ty(context),
                                 builder.CreateStructGEP(union_st, self, 0),
                                 "tag");
  auto *done = llvm::BasicBlock::Create(context, "done", fn);
  auto &alts = std::get<UnionTypeInfo>(sem->detail).alternatives;
  auto *sw = builder.CreateSwitch(tag, done, alts.size());
  for (size_t i = 0; i < alts.size(); ++i) {
    if (union_alt_is_boxed(alts[i]) || !holds_references(alts[i]))
      continue;
    auto *arm = llvm::BasicBlock::Create(context, "alt", fn, done);
    sw->addCase(builder.getInt8(static_cast<uint8_t>(i)), arm);
    builder.SetInsertPoint(arm);
    walk_slot(builder.CreateStructGEP(union_st, self, 1, "payload"), alts[i],
              retain);
    builder.CreateBr(done);
  }
  builder.SetInsertPoint(done);
  builder.CreateRetVoid();
  builder.restoreIP(saved);
  return fn;
}

// What a slot of `slot` type holds: a counted value through its pointer, a
// struct or union through its walk.
void CodeGen::walk_slot(llvm::Value *addr, const TypePtr &slot, bool retain) {
  auto sem = unwrap_alias(slot);
  if (is_counted(sem)) {
    auto *val = builder.CreateLoad(llvm::PointerType::getUnqual(context), addr);
    retain ? emit_retain(val, sem) : emit_release(val, sem);
  } else if (walks_references(sem)) {
    if (auto *walker = ownership_fn(sem, retain))
      builder.CreateCall(walker, {addr});
  }
}

} // namespace saga

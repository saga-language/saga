// Copyright 2026 Rob Thornton
// SPDX-License-Identifier: MIT

// Where owned values end. A local ends with the block that declares it. A
// temporary — an owned value whose consumer only reads it: a receiver, an
// operand, the value a statement discards — ends with the full expression that
// made it, and is held in a slot of its own that is null outside it, so one
// made on only some paths through it, in a branch or past a `?`, is released
// on exactly those: releasing null is a no-op. Clang keeps a conditional
// temporary's cleanup the same way. A jump out of a scope — `return`,
// `break`, `next`, a `?` escape — releases what the scopes it leaves hold.

#include "ir/codegen.hpp"

#include <llvm/IR/Constants.h>

namespace saga {

CodeGen::LocalScope::LocalScope(CodeGen &cg)
    : cg_(cg), depth_(cg.managed_locals.size()) {}

CodeGen::LocalScope::~LocalScope() { cg_.end_local_scope(depth_); }

void CodeGen::end_local_scope(size_t depth) {
  release_locals(depth);
  managed_locals.resize(depth);
}

CodeGen::CleanupDepth CodeGen::cleanup_depth() const {
  return {temporaries_.size(), managed_locals.size()};
}

void CodeGen::release_to(const CleanupDepth &depth) {
  release_temporaries(depth.temporaries);
  release_locals(depth.locals);
}

// Last declared, first released.
void CodeGen::release_locals(size_t depth) {
  if (!builder.GetInsertBlock() || builder.GetInsertBlock()->getTerminator())
    return;
  for (size_t i = managed_locals.size(); i > depth; --i)
    release_local(managed_locals[i - 1]);
}

// A closeable struct is closed, then lets go of what it holds as any struct.
void CodeGen::release_local(const ManagedLocal &local) {
  switch (local.kind) {
  case ManagedKind::Closeable:
    emit_close_call(local.slot);
    emit_release(local.slot, local.sem);
    return;
  case ManagedKind::Struct:
    emit_release(local.slot, local.sem);
    return;
  case ManagedKind::Task:
    builder.CreateCall(module->getFunction("saga_task_drop"),
                       {builder.CreateLoad(local.slot->getAllocatedType(),
                                           local.slot)});
    return;
  case ManagedKind::Counted:
    emit_release(builder.CreateLoad(local.slot->getAllocatedType(),
                                    local.slot),
                 local.sem);
    return;
  }
}

CodeGen::FullExpression::FullExpression(CodeGen &cg)
    : cg_(cg), depth_(cg.temporaries_.size()) {}

CodeGen::FullExpression::~FullExpression() {
  cg_.release_temporaries(depth_);
  cg_.temporaries_.resize(depth_);
}

// A struct or union is held by address, so a method that writes through it
// before the expression ends is seen by the release.
void CodeGen::hold_temporary(llvm::Value *val, const TypePtr &sem) {
  auto shape = unwrap_alias(sem);
  if (!val || !holds_references(shape))
    return;
  if (!is_counted(shape))
    val = spill_aggregate(val, "temp.value");
  auto *slot = create_entry_alloca(builder.GetInsertBlock()->getParent(),
                                   "temp", val->getType());
  llvm::IRBuilder<> entry(slot->getNextNode());
  entry.CreateStore(llvm::Constant::getNullValue(val->getType()), slot);
  builder.CreateStore(val, slot);
  temporaries_.push_back({slot, shape});
}

void CodeGen::hold_if_owned(llvm::Value *val, const TypePtr &sem,
                            const Node &source) {
  if (val && value_ownership(source) == Ownership::Owned)
    hold_temporary(val, sem);
}

llvm::Value *CodeGen::emit_borrowed(const Node &node) {
  auto *val = emit_expr(node);
  hold_if_owned(val, semantic_type(node), node);
  return val;
}

llvm::Value *CodeGen::emit_borrowed_operand(const Node &node) {
  auto *val = emit_operand(node);
  hold_if_owned(val, operand_type(node), node);
  return val;
}

// Every way out of a full expression releases what it holds — its end, and a
// `return`, `break` or `?` that leaves early — and leaves the slots null, so
// no path releases a value twice.
void CodeGen::release_temporaries(size_t depth) {
  if (!builder.GetInsertBlock() || builder.GetInsertBlock()->getTerminator())
    return;
  for (size_t i = temporaries_.size(); i > depth; --i) {
    auto &temp = temporaries_[i - 1];
    auto *ll = temp.slot->getAllocatedType();
    emit_release(builder.CreateLoad(ll, temp.slot), temp.sem);
    builder.CreateStore(llvm::Constant::getNullValue(ll), temp.slot);
  }
}

// A loop's body or a task's: each statement a full expression of its own, and
// the block's locals ending with it.
void CodeGen::emit_body(const BlockNode &block) {
  LocalScope scope(*this);
  for (auto &stmt : block.stmts) {
    if (builder.GetInsertBlock()->getTerminator())
      return;
    emit_statement(*stmt);
  }
}

void CodeGen::emit_statement(const Node &stmt) {
  FullExpression full(*this);
  auto *val = emit_root_expr(stmt);
  hold_if_owned(val, root_expr_type(stmt), stmt);
}

llvm::Value *CodeGen::emit_condition(const Node &cond) {
  FullExpression full(*this);
  return as_condition(emit_expr(cond));
}

} // namespace saga

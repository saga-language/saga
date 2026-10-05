// Copyright 2026 Rob Thornton
// SPDX-License-Identifier: MIT

// Where a conditional's branches meet. Each branch hands its value over in the
// conditional's own type and form — an aggregate as an address — so the merge
// is one PHI whatever shape each branch produced it in.

#include "ir/codegen.hpp"
#include "util/internal_error.hpp"

namespace saga {

namespace {
bool yields_value(const TypePtr &result) {
  return result && result->kind != TypeKind::Void &&
         !is_invalid_type(result) && !is_unknown_type(result);
}
} // namespace

BranchJoin CodeGen::open_join(const std::string &name, const TypePtr &result,
                              Ownership merged) {
  BranchJoin join;
  join.merge = llvm::BasicBlock::Create(context, name);
  join.result = yields_value(result) ? materialize_untyped(result) : nullptr;
  join.merged = merged;
  return join;
}

// A branch that already returned or broke never reaches the merge.
void CodeGen::close_branch(BranchJoin &join, llvm::Value *val,
                           const TypePtr &val_sem, const Node *source) {
  if (builder.GetInsertBlock()->getTerminator())
    return;
  join.reached = true;
  if (join.result)
    join.incoming.push_back(
        {join_value(join, val, val_sem, source), builder.GetInsertBlock()});
  builder.CreateBr(join.merge);
}

// A borrowed value takes a reference when the merge is owned, and when it is
// boxed, since the box takes over what it is given.
llvm::Value *CodeGen::join_value(const BranchJoin &join, llvm::Value *val,
                                 const TypePtr &val_sem, const Node *source) {
  if (!val || val->getType()->isVoidTy())
    return emit_zero_value(join.result);
  bool takes_reference =
      join.merged == Ownership::Owned || boxes_into(val_sem, join.result);
  if (takes_reference && branch_value_ownership(source, join.result) ==
                             Ownership::Borrowed)
    emit_retain(val, unwrap_alias(val_sem));
  auto *placed = coerce_to(val, val_sem, join.result);
  if (llvm_type(join.result)->isStructTy())
    return spill_aggregate(placed, "join.spill");
  return placed;
}

llvm::Value *CodeGen::finish_join(BranchJoin &join, const std::string &name) {
  auto *func = builder.GetInsertBlock()->getParent();
  func->insert(func->end(), join.merge);
  builder.SetInsertPoint(join.merge);
  if (!join.reached) {
    builder.CreateUnreachable();
    return nullptr;
  }
  if (join.incoming.empty())
    return nullptr;

  auto *ty = join.incoming.front().first->getType();
  auto *phi = builder.CreatePHI(ty, join.incoming.size(), name);
  for (auto &[val, block] : join.incoming) {
    if (val->getType() != ty)
      internal_error("the branches of '" + name + "' reached the merge as "
                     "different LLVM types, though each was placed in the "
                     "conditional's own type");
    phi->addIncoming(val, block);
  }
  return phi;
}

// An aggregate's zero stays in its slot, the form a join hands one over in.
llvm::Value *CodeGen::emit_zero_value(const TypePtr &sem) {
  auto *ll = storage_type(sem);
  auto *slot = create_entry_alloca(builder.GetInsertBlock()->getParent(),
                                   "zero", ll);
  zero_fill(slot, sem, ll);
  if (ll->isStructTy())
    return slot;
  return builder.CreateLoad(ll, slot, "zero.val");
}

} // namespace saga

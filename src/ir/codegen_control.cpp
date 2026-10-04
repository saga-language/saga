// Copyright 2026 Rob Thornton
// SPDX-License-Identifier: MIT

// Group and if/else expressions. Each branch of an if ends in the one join
// the conditional's value is taken from (codegen_join.cpp).

#include "ir/codegen.hpp"

#include <llvm/IR/Constants.h>

namespace saga {

llvm::Value *CodeGen::emit_group_expr(const GroupExprNode &node) {
  return emit_expr(*node.inner);
}

llvm::Value *CodeGen::as_condition(llvm::Value *val) {
  if (!val || val->getType()->isIntegerTy(1))
    return val;
  return builder.CreateICmpNE(
      val, llvm::Constant::getNullValue(val->getType()), "tobool");
}

void CodeGen::start_block(llvm::BasicBlock *block) {
  builder.GetInsertBlock()->getParent()->insert(
      builder.GetInsertBlock()->getParent()->end(), block);
  builder.SetInsertPoint(block);
}

llvm::AllocaInst *CodeGen::narrow_local(const std::string &name,
                                        const TypePtr &from,
                                        const TypePtr &to) {
  auto it = locals.find(name);
  if (it == locals.end() || !to)
    return nullptr;

  llvm::AllocaInst *slot = nullptr;
  if (to->kind == TypeKind::Union) {
    // More than one alternative survives, so the value stays a union — a
    // narrower one, with its own tag numbering.
    slot = llvm::dyn_cast_or_null<llvm::AllocaInst>(
        emit_union_convert(it->second, from, to));
  } else if (auto *val = emit_union_extract(it->second, to, from)) {
    auto *func = builder.GetInsertBlock()->getParent();
    slot = create_entry_alloca(func, name + ".narrowed", storage_type(to));
    builder.CreateStore(val, slot);
  }
  if (!slot || slot == it->second)
    return nullptr;

  auto *displaced = it->second;
  locals[name] = slot;
  return displaced;
}

// `if v is T` narrows `v` to T in the then branch.
std::optional<CodeGen::Narrowing>
CodeGen::if_narrowing(const IfExprNode &node) {
  auto *is_expr = std::get_if<IsExpr>(&node.condition->data);
  if (!is_expr)
    return std::nullopt;
  auto *id = std::get_if<IdentifierNode>(&is_expr->value->data);
  auto from = semantic_type(*is_expr->value);
  auto to = semantic_type(*is_expr->type);
  if (!id || !from || from->kind != TypeKind::Union || !to)
    return std::nullopt;
  return Narrowing{std::string(id->name), from, to};
}

// The test failed, so what is left is the union minus the type tested for —
// the same answer the analyzer narrowed the else-scope with.
std::optional<CodeGen::Narrowing>
CodeGen::else_narrowing(const std::optional<Narrowing> &then) {
  if (!then)
    return std::nullopt;
  return Narrowing{then->name, then->from, union_without(then->from, then->to)};
}

llvm::Value *CodeGen::emit_if_expr(const IfExprNode &node, const Node &parent) {
  if (node.init)
    emit_expr(**node.init);
  auto *cond = as_condition(emit_expr(*node.condition));
  if (!cond)
    return nullptr;

  auto *then_bb = llvm::BasicBlock::Create(
      context, "then", builder.GetInsertBlock()->getParent());
  auto *else_bb = llvm::BasicBlock::Create(context, "else");
  builder.CreateCondBr(cond, then_bb, else_bb);

  auto join = open_join("merge", semantic_type(parent));
  auto narrowing = if_narrowing(node);
  builder.SetInsertPoint(then_bb);
  emit_if_branch(join, node.then_block.get(), narrowing);
  start_block(else_bb);
  emit_if_branch(join, node.else_block ? node.else_block->get() : nullptr,
                 else_narrowing(narrowing));
  return finish_join(join, "ifval");
}

// A missing else is a branch with no value, which yields the zero value.
void CodeGen::emit_if_branch(BranchJoin &join, const Node *body,
                             const std::optional<Narrowing> &narrowing) {
  if (!body) {
    close_branch(join, nullptr, nullptr);
    return;
  }
  auto *displaced = narrowing ? narrow_local(narrowing->name, narrowing->from,
                                             narrowing->to)
                              : nullptr;
  auto &block = std::get<BlockNode>(body->data);
  auto *val = emit_block(block);
  if (displaced)
    locals[narrowing->name] = displaced;
  close_branch(join, val, block_result_type(block));
}

} // namespace saga

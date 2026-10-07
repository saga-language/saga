// Copyright 2026 Rob Thornton
// SPDX-License-Identifier: MIT

#include "ir/codegen.hpp"

#include <llvm/IR/Constants.h>

namespace saga {

// An accumulator with no initializer starts at the zero value of its type,
// which is what makes `|acc| { acc += x }` a sum.
void CodeGen::seed_accumulator(llvm::Value *slot, const AccumulatorNode &acc,
                               const TypePtr &sem, llvm::Type *ll) {
  llvm::Value *val = acc.init ? emit_root_expr(**acc.init) : nullptr;
  if (!val) {
    zero_fill(slot, sem, ll);
    return;
  }

  auto val_sem = root_expr_type(**acc.init);
  retain_if_borrowed(val, unwrap_alias(val_sem), **acc.init);
  val = coerce_to(val, val_sem, sem);

  if (val->getType()->isPointerTy() && ll->isStructTy())
    val = builder.CreateLoad(ll, val, "acc.seed");
  builder.CreateStore(val, slot);
}

llvm::Value *CodeGen::emit_for_expr(const ForExprNode &node,
                                    const Node &parent) {
  auto *func = builder.GetInsertBlock()->getParent();

  auto for_sem = semantic_type(parent);

  // Accumulator setup: allocate a local seeded to the accumulator's own type,
  // bind it as `acc`, and load+return it after the loop exits.  Its type is
  // not the for-expression's — an accumulator names a slot the loop writes
  // across iterations, and a `break` value would give the loop a wider type
  // than that slot holds.
  llvm::AllocaInst *acc_alloca = nullptr;
  llvm::Type *acc_ll = nullptr;
  if (node.accumulator) {
    auto acc_sem = semantic_type(*node.accumulator->name);
    acc_ll = storage_type(acc_sem);
    if (acc_ll) {
      auto &ident = std::get<IdentifierNode>(node.accumulator->name->data);
      std::string acc_name(ident.name);
      acc_alloca = create_entry_alloca(func, acc_name, acc_ll);
      seed_accumulator(acc_alloca, *node.accumulator, acc_sem, acc_ll);
      locals[acc_name] = acc_alloca;
    }
  }

  // break-with-value: if the recorded type is `T | Error`, allocate a
  // union slot pre-filled with the err tag.  break codegen will wrap
  // its value with the ok tag and store before branching.
  llvm::AllocaInst *break_result = nullptr;
  TypePtr break_value_type;
  if (!node.accumulator && for_sem && for_sem->kind == TypeKind::Union &&
      is_impure_union(for_sem)) {
    auto *union_st = get_union_llvm_type(for_sem);
    if (union_st) {
      auto &uinfo = std::get<UnionTypeInfo>(for_sem->detail);
      int err_tag = -1;
      for (size_t i = 0; i < uinfo.alternatives.size(); ++i) {
        auto &alt = uinfo.alternatives[i];
        if (is_error_valued(alt))
          err_tag = static_cast<int>(i);
        else if (alt)
          break_value_type = alt;
      }
      if (err_tag >= 0 && break_value_type) {
        break_result = create_entry_alloca(func, "for.result", union_st);
        builder.CreateStore(llvm::Constant::getNullValue(union_st),
                            break_result);
        auto *tag_gep =
            builder.CreateStructGEP(union_st, break_result, 0, "for.tag");
        builder.CreateStore(
            llvm::ConstantInt::get(llvm::Type::getInt8Ty(context), err_tag),
            tag_gep);
      }
    }
  }

  ForLoopBlocks bbs;
  bbs.cond_bb = llvm::BasicBlock::Create(context, "for.cond", func);
  bbs.body_bb = llvm::BasicBlock::Create(context, "for.body");
  bbs.update_bb = llvm::BasicBlock::Create(context, "for.update");
  bbs.exit_bb = llvm::BasicBlock::Create(context, "for.exit");

  LoopContext frame{bbs.exit_bb, bbs.update_bb, break_result,
                    break_result ? for_sem : TypePtr{},
                    break_value_type};
  loop_stack.push_back(frame);

  if (!node.mode) {
    emit_for_infinite(node, bbs);
  } else {
    auto &mode_node = *node.mode;
    if (auto *iter = std::get_if<ForIterClauseNode>(&mode_node->data))
      emit_for_c_style(node, *iter, bbs);
    else if (auto *range = std::get_if<ForRangeClauseNode>(&mode_node->data))
      emit_for_range(node, *range, bbs);
    else
      emit_for_condition(node, *mode_node, bbs);
  }

  loop_stack.pop_back();
  start_block(bbs.exit_bb);

  if (acc_alloca && acc_ll)
    return builder.CreateLoad(acc_ll, acc_alloca, "for.acc");
  if (break_result)
    return break_result;
  return nullptr;
}

// An actor's loop counts toward its reductions, so a long one yields.
void CodeGen::enter_loop_body(const ForLoopBlocks &bbs) {
  start_block(bbs.body_bb);
  if (current_actor)
    builder.CreateCall(module->getFunction("saga_reduction_tick"),
                       {current_actor});
}

void CodeGen::finish_loop_body(const ForExprNode &node,
                               llvm::BasicBlock *next) {
  emit_block(std::get<BlockNode>(node.body->data));
  if (!builder.GetInsertBlock()->getTerminator())
    builder.CreateBr(next);
}

void CodeGen::emit_for_infinite(const ForExprNode &node,
                                const ForLoopBlocks &bbs) {
  builder.CreateBr(bbs.body_bb);

  // cond_bb is the "next" trampoline that loops back to body.
  builder.SetInsertPoint(bbs.cond_bb);
  builder.CreateBr(bbs.body_bb);
  loop_stack.back().next_bb = bbs.cond_bb;

  enter_loop_body(bbs);
  finish_loop_body(node, bbs.cond_bb);
}

void CodeGen::emit_for_c_style(const ForExprNode &node,
                               const ForIterClauseNode &iter,
                               const ForLoopBlocks &bbs) {

  emit_expr(*iter.init);
  builder.CreateBr(bbs.cond_bb);

  builder.SetInsertPoint(bbs.cond_bb);
  auto *cond_val = as_condition(emit_expr(*iter.condition));
  if (cond_val)
    builder.CreateCondBr(cond_val, bbs.body_bb, bbs.exit_bb);
  else
    builder.CreateBr(bbs.body_bb);

  enter_loop_body(bbs);
  finish_loop_body(node, bbs.update_bb);

  start_block(bbs.update_bb);
  emit_expr(*iter.update);
  builder.CreateBr(bbs.cond_bb);
}

void CodeGen::emit_for_condition(const ForExprNode &node, const Node &mode,
                                 const ForLoopBlocks &bbs) {
  builder.CreateBr(bbs.cond_bb);

  builder.SetInsertPoint(bbs.cond_bb);
  auto *cond_val = as_condition(emit_expr(mode));
  if (cond_val)
    builder.CreateCondBr(cond_val, bbs.body_bb, bbs.exit_bb);
  else
    builder.CreateBr(bbs.body_bb);

  enter_loop_body(bbs);
  finish_loop_body(node, bbs.update_bb);

  start_block(bbs.update_bb);
  builder.CreateBr(bbs.cond_bb);
}

// The bounds of a counted loop, not a collection to build and then walk.
void CodeGen::emit_for_range_counted(const ForExprNode &node,
                                     const ForRangeClauseNode &range,
                                     const RangeNode &rng,
                                     const ForLoopBlocks &bbs) {
  auto *func = builder.GetInsertBlock()->getParent();
  auto *low = emit_expr(*rng.low);
  auto *high = emit_expr(*rng.high);
  if (!low || !high) {
    builder.CreateBr(bbs.exit_bb);
    return;
  }

  auto *counter_ll = low->getType();
  auto *cur = create_entry_alloca(func, std::string(range.vars[0].name),
                                  counter_ll);
  builder.CreateStore(low, cur);
  locals[std::string(range.vars[0].name)] = cur;

  builder.CreateBr(bbs.cond_bb);
  builder.SetInsertPoint(bbs.cond_bb);
  auto *v = builder.CreateLoad(counter_ll, cur, "range.v");
  builder.CreateCondBr(builder.CreateICmpSLT(v, high, "range.cmp"),
                       bbs.body_bb, bbs.exit_bb);

  enter_loop_body(bbs);
  finish_loop_body(node, bbs.update_bb);

  start_block(bbs.update_bb);
  auto *upd = builder.CreateLoad(counter_ll, cur, "range.v");
  builder.CreateStore(
      builder.CreateAdd(upd, llvm::ConstantInt::get(counter_ll, 1),
                        "range.next"),
      cur);
  builder.CreateBr(bbs.cond_bb);
}

void CodeGen::emit_for_range(const ForExprNode &node,
                             const ForRangeClauseNode &range,
                             const ForLoopBlocks &bbs) {
  if (auto *rng = std::get_if<RangeNode>(&range.iterable->data)) {
    emit_for_range_counted(node, range, *rng, bbs);
    return;
  }

  auto *iterable = emit_expr(*range.iterable);
  if (!iterable) {
    builder.CreateBr(bbs.exit_bb);
    return;
  }
  auto iter_sem = semantic_type(*range.iterable);
  if (!iter_sem) {
    builder.CreateBr(bbs.exit_bb);
    return;
  }

  switch (iter_sem->kind) {
  case TypeKind::Array:
    emit_for_range_array(node, range, iterable, iter_sem, bbs);
    return;
  case TypeKind::Map:
    emit_for_range_map(node, range, iterable, iter_sem, bbs);
    return;
  case TypeKind::Struct: {
    auto &st_info = std::get<StructTypeInfo>(iter_sem->detail);
    if (st_info.name == "Task")
      emit_for_range_task(node, range, st_info, bbs);
    else
      emit_for_range_iterable_struct(node, range, iterable, iter_sem, bbs);
    return;
  }
  default:
    builder.CreateBr(bbs.exit_bb);
    return;
  }
}

} // namespace saga

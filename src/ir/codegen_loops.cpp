// Copyright 2026 Rob Thornton
// SPDX-License-Identifier: MIT

#include "ir/codegen.hpp"
#include "util/internal_error.hpp"

#include <llvm/IR/Constants.h>

#include <algorithm>

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

// An accumulator names a slot the loop writes across iterations, typed as the
// accumulator rather than the loop: a `break` value would make the loop's type
// wider than that slot holds.
llvm::Value *CodeGen::emit_for_expr(const ForExprNode &node,
                                    const Node &parent) {
  auto *func = builder.GetInsertBlock()->getParent();
  auto *acc = node.accumulator ? open_accumulator(*node.accumulator) : nullptr;

  ForLoopBlocks bbs;
  bbs.cond_bb = llvm::BasicBlock::Create(context, "for.cond", func);
  bbs.body_bb = llvm::BasicBlock::Create(context, "for.body");
  bbs.update_bb = llvm::BasicBlock::Create(context, "for.update");
  bbs.exit_bb = llvm::BasicBlock::Create(context, "for.exit");

  auto for_sem = semantic_type(parent);
  auto value_type = break_value_type(node, for_sem);
  auto *break_result = value_type ? open_break_result(for_sem) : nullptr;
  loop_stack.push_back({bbs.exit_bb, bbs.update_bb, break_result,
                        value_type ? for_sem : TypePtr{}, value_type});
  emit_loop(node, bbs);
  loop_stack.pop_back();
  start_block(bbs.exit_bb);

  if (acc)
    return builder.CreateLoad(acc->getAllocatedType(), acc, "for.acc");
  return break_result;
}

void CodeGen::emit_loop(const ForExprNode &node, const ForLoopBlocks &bbs) {
  if (!node.mode)
    return emit_for_infinite(node, bbs);
  auto &mode = *node.mode;
  if (auto *iter = std::get_if<ForIterClauseNode>(&mode->data))
    emit_for_c_style(node, *iter, bbs);
  else if (auto *range = std::get_if<ForRangeClauseNode>(&mode->data))
    emit_for_range(node, *range, bbs);
  else
    emit_for_condition(node, *mode, bbs);
}

llvm::AllocaInst *CodeGen::open_accumulator(const AccumulatorNode &acc) {
  auto acc_sem = semantic_type(*acc.name);
  auto *acc_ll = storage_type(acc_sem);
  std::string name(std::get<IdentifierNode>(acc.name->data).name);
  auto *slot = create_entry_alloca(builder.GetInsertBlock()->getParent(),
                                   name, acc_ll);
  seed_accumulator(slot, acc, acc_sem, acc_ll);
  locals[name] = slot;
  return slot;
}

// A for-expression typed `T | Error` yields what a `break` hands it, and an
// error when it runs out instead.
TypePtr CodeGen::break_value_type(const ForExprNode &node,
                                  const TypePtr &for_sem) {
  if (node.accumulator || !for_sem || for_sem->kind != TypeKind::Union ||
      !is_impure_union(for_sem))
    return nullptr;
  TypePtr value;
  bool has_error = false;
  for (auto &alt : std::get<UnionTypeInfo>(for_sem->detail).alternatives) {
    if (is_error_valued(alt))
      has_error = true;
    else if (alt)
      value = alt;
  }
  return has_error ? value : nullptr;
}

llvm::AllocaInst *CodeGen::open_break_result(const TypePtr &for_sem) {
  auto *union_st = get_union_llvm_type(for_sem);
  auto *slot = create_entry_alloca(builder.GetInsertBlock()->getParent(),
                                   "for.result", union_st);
  builder.CreateStore(llvm::Constant::getNullValue(union_st), slot);
  auto &alts = std::get<UnionTypeInfo>(for_sem->detail).alternatives;
  auto err = std::find_if(alts.rbegin(), alts.rend(), is_error_valued);
  auto tag = static_cast<uint8_t>(alts.rend() - err - 1);
  builder.CreateStore(builder.getInt8(tag),
                      builder.CreateStructGEP(union_st, slot, 0, "for.tag"));
  return slot;
}

// `break <value>` from a for-expression typed `T | Error` leaves the value in
// the loop's result slot, which owns it as a binding would.
void CodeGen::emit_break(const BreakNode &node) {
  if (loop_stack.empty())
    return;
  auto &frame = loop_stack.back();
  if (frame.result_alloca && !node.values.empty() && frame.result_value_type)
    store_break_value(frame, *node.values[0]);
  release_temporaries(frame.temporaries_depth);
  builder.CreateBr(frame.break_bb);
}

void CodeGen::store_break_value(const LoopContext &frame, const Node &value) {
  auto *val = emit_expr(value);
  if (!val)
    return;
  retain_if_borrowed(val, semantic_type(value), value);
  auto *wrapped =
      coerce_to(val, frame.result_value_type, frame.result_union_type);
  if (!wrapped)
    internal_error("a break value could not be placed in its loop's result");
  auto *union_st = get_union_llvm_type(frame.result_union_type);
  auto al = align_of(union_st);
  builder.CreateMemCpy(frame.result_alloca, al, wrapped, al,
                       size_of(union_st));
}

void CodeGen::emit_next() {
  if (loop_stack.empty())
    return;
  release_temporaries(loop_stack.back().temporaries_depth);
  builder.CreateBr(loop_stack.back().next_bb);
}

// `break` and `next` leave the body, not what the loop walks, so they release
// only what the body holds. An actor's loop counts toward its reductions, so
// a long one yields.
void CodeGen::enter_loop_body(const ForLoopBlocks &bbs) {
  loop_stack.back().temporaries_depth = temporaries_.size();
  start_block(bbs.body_bb);
  if (current_actor)
    builder.CreateCall(module->getFunction("saga_reduction_tick"),
                       {current_actor});
}

void CodeGen::finish_loop_body(const ForExprNode &node,
                               llvm::BasicBlock *next) {
  emit_body(std::get<BlockNode>(node.body->data));
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

  emit_statement(*iter.init);
  builder.CreateBr(bbs.cond_bb);

  builder.SetInsertPoint(bbs.cond_bb);
  auto *cond_val = emit_condition(*iter.condition);
  if (cond_val)
    builder.CreateCondBr(cond_val, bbs.body_bb, bbs.exit_bb);
  else
    builder.CreateBr(bbs.body_bb);

  enter_loop_body(bbs);
  finish_loop_body(node, bbs.update_bb);

  start_block(bbs.update_bb);
  emit_statement(*iter.update);
  builder.CreateBr(bbs.cond_bb);
}

void CodeGen::emit_for_condition(const ForExprNode &node, const Node &mode,
                                 const ForLoopBlocks &bbs) {
  builder.CreateBr(bbs.cond_bb);

  builder.SetInsertPoint(bbs.cond_bb);
  auto *cond_val = emit_condition(mode);
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

  auto *iterable = emit_borrowed(*range.iterable);
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

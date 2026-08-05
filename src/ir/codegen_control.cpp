// Copyright 2026 Rob Thornton
// SPDX-License-Identifier: MIT

// Branching constructs: group, if/else and switch expressions. Each yields a
// value, so every path has to converge on one PHI.

#include "ir/codegen.hpp"

#include <llvm/IR/Constants.h>

namespace saga {

// ===========================================================================
// Group expression
// ===========================================================================

llvm::Value *CodeGen::emit_group_expr(const GroupExprNode &node) {
  return emit_expr(*node.inner);
}

// ===========================================================================
// If/else expression
// ===========================================================================

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
    slot = create_entry_alloca(func, name + ".narrowed", llvm_type(to));
    builder.CreateStore(val, slot);
  }
  if (!slot || slot == it->second)
    return nullptr;

  auto *displaced = it->second;
  locals[name] = slot;
  return displaced;
}

llvm::Value *CodeGen::emit_if_expr(const IfExprNode &node, const Node &parent) {
  auto *cond = emit_expr(*node.condition);
  if (!cond)
    return nullptr;

  // When the if is used as a union-typed value, its branches may yield
  // different member types (e.g. `if fail { NetworkError{...} } else { 200 }`
  // is `int | error`). Wrap each branch into union memory so they share a
  // pointer type for the merge PHI.
  auto if_sem = semantic_type(parent);
  bool if_is_union = if_sem && if_sem->kind == TypeKind::Union;
  auto wrap_branch = [&](llvm::Value *v, const BlockNode &b) -> llvm::Value * {
    if (!if_is_union || !v || v->getType()->isVoidTy())
      return v;
    // The branch already returned/broke — its value can't reach the merge, so
    // there's nothing to wrap (and the block is terminated).
    if (builder.GetInsertBlock()->getTerminator())
      return v;
    if (auto *w = as_union_ptr(v, block_result_type(b), if_sem))
      return w;
    return v;
  };

  // If the condition is an i64 (from a comparison that got widened), truncate
  // to i1.  If it's already i1, use it directly.
  if (!cond->getType()->isIntegerTy(1)) {
    cond = builder.CreateICmpNE(
        cond, llvm::Constant::getNullValue(cond->getType()), "tobool");
  }

  auto *func = builder.GetInsertBlock()->getParent();

  // Create basic blocks.
  auto *then_bb = llvm::BasicBlock::Create(context, "then", func);
  auto *merge_bb = llvm::BasicBlock::Create(context, "merge");
  llvm::BasicBlock *else_bb = nullptr;

  if (node.else_block) {
    else_bb = llvm::BasicBlock::Create(context, "else");
    builder.CreateCondBr(cond, then_bb, else_bb);
  } else {
    builder.CreateCondBr(cond, then_bb, merge_bb);
  }

  // ── Detect type-test pattern for narrowing ─────────────────────────
  // `if value is Type` extracts the narrowed value from the union into the
  // then-block.
  std::string narrowed_var_name;
  TypePtr narrow_union_sem;
  TypePtr narrow_target_sem;
  llvm::AllocaInst *saved_alloca = nullptr;
  if (auto *is_expr = std::get_if<IsExpr>(&node.condition->data)) {
    if (auto *lhs_id = std::get_if<IdentifierNode>(&is_expr->value->data)) {
      auto lhs_sem = semantic_type(*is_expr->value);
      auto rhs_sem = semantic_type(*is_expr->type);
      if (lhs_sem && lhs_sem->kind == TypeKind::Union && rhs_sem) {
        narrowed_var_name = std::string(lhs_id->name);
        narrow_union_sem = lhs_sem;
        narrow_target_sem = rhs_sem;
      }
    }
  }

  // ── Then block ─────────────────────────────────────────────────────
  builder.SetInsertPoint(then_bb);

  if (!narrowed_var_name.empty())
    saved_alloca =
        narrow_local(narrowed_var_name, narrow_union_sem, narrow_target_sem);

  auto &then_block = std::get<BlockNode>(node.then_block->data);
  auto *then_val = emit_block(then_block);
  then_val = wrap_branch(then_val, then_block);

  // Restore the original alloca after the then-block.
  if (saved_alloca) {
    locals[narrowed_var_name] = saved_alloca;
  }

  // If the then block didn't terminate, branch to merge.
  bool then_terminated = builder.GetInsertBlock()->getTerminator() != nullptr;
  if (!then_terminated)
    builder.CreateBr(merge_bb);
  // Record the actual ending block (may differ from then_bb if sub-blocks
  // were created).
  auto *then_end_bb = builder.GetInsertBlock();

  // ── Else block ─────────────────────────────────────────────────────
  llvm::Value *else_val = nullptr;
  llvm::BasicBlock *else_end_bb = nullptr;
  bool else_terminated = false;

  if (else_bb) {
    func->insert(func->end(), else_bb);
    builder.SetInsertPoint(else_bb);

    // The test failed, so what is left is the union minus the type tested for
    // — the same answer the analyzer narrowed the else-scope with.
    llvm::AllocaInst *saved_else = nullptr;
    if (!narrowed_var_name.empty())
      saved_else = narrow_local(narrowed_var_name, narrow_union_sem,
                                union_without(narrow_union_sem,
                                              narrow_target_sem));

    auto &else_block = std::get<BlockNode>((*node.else_block)->data);
    else_val = emit_block(else_block);
    else_val = wrap_branch(else_val, else_block);

    if (saved_else)
      locals[narrowed_var_name] = saved_else;

    else_terminated = builder.GetInsertBlock()->getTerminator() != nullptr;
    if (!else_terminated)
      builder.CreateBr(merge_bb);
    else_end_bb = builder.GetInsertBlock();
  }

  // ── Merge block ────────────────────────────────────────────────────
  // The merge block is reachable if at least one branch doesn't terminate.
  // With no else block, the false branch always reaches merge.
  bool merge_reachable = !then_terminated || !else_terminated;
  if (!node.else_block)
    merge_reachable = true; // false-branch falls through to merge

  // Only emit merge if both branches with else terminated — then it's dead.
  if (node.else_block && then_terminated && else_terminated) {
    // Both branches returned/broke — merge is unreachable.
    // Don't insert it; just leave the insert point at a terminated block.
    // We need a valid insert point though, so add it and mark unreachable.
    func->insert(func->end(), merge_bb);
    builder.SetInsertPoint(merge_bb);
    builder.CreateUnreachable();
    return nullptr;
  }

  func->insert(func->end(), merge_bb);
  builder.SetInsertPoint(merge_bb);

  // Build a PHI node if both branches produce a value of the same type. A void
  // one is not a value: an `if` in statement position has nothing to merge.
  if (then_val && else_val && then_val->getType() == else_val->getType() &&
      !then_val->getType()->isVoidTy() && !then_terminated &&
      !else_terminated) {
    auto *phi = builder.CreatePHI(then_val->getType(), 2, "ifval");
    phi->addIncoming(then_val, then_end_bb);
    phi->addIncoming(else_val, else_end_bb);
    return phi;
  }

  // If there's no else branch but the then branch produced a non-void value,
  // create a PHI with the zero value on the false path.
  if (then_val && !node.else_block && !then_terminated &&
      !then_val->getType()->isVoidTy()) {
    auto *zero = llvm::Constant::getNullValue(then_val->getType());
    // The false path comes directly from the entry (before the branch).
    // We need the predecessor of merge that isn't then_end_bb.
    // That's the block that had the conditional branch (entry).
    auto *false_pred = merge_bb->getSinglePredecessor();
    if (!false_pred) {
      // merge has two predecessors: then_end_bb and the original entry.
      for (auto *pred : llvm::predecessors(merge_bb)) {
        if (pred != then_end_bb) {
          false_pred = pred;
          break;
        }
      }
    }
    if (false_pred) {
      auto *phi = builder.CreatePHI(then_val->getType(), 2, "ifval");
      phi->addIncoming(then_val, then_end_bb);
      phi->addIncoming(zero, false_pred);
      return phi;
    }
  }

  return then_val;
}

// ===========================================================================
// Switch expression
// ===========================================================================

llvm::Value *CodeGen::emit_switch_expr(const SwitchExprNode &node) {
  auto *subject_val = emit_expr(*node.subject);
  if (!subject_val)
    return nullptr;

  auto subject_sem = semantic_type(*node.subject);
  bool is_string = subject_sem && subject_sem->kind == TypeKind::String;
  bool is_union = subject_sem && subject_sem->kind == TypeKind::Union;
  bool is_int_like = !is_string && !is_union;

  auto *func = builder.GetInsertBlock()->getParent();
  auto *merge_bb = llvm::BasicBlock::Create(context, "sw.merge");

  // Collect case info for building PHI nodes.
  struct CaseResult {
    llvm::Value *value;
    llvm::BasicBlock *block;
    bool terminated;
  };
  std::vector<CaseResult> case_results;

  if (is_union) {
    // ── Union type matching: switch on the tag byte ─────────────────
    auto *union_st = get_union_llvm_type(subject_sem);
    llvm::Value *union_ptr = subject_val;

    auto *tag_gep = builder.CreateStructGEP(union_st, union_ptr, 0,
                                             "sw.union.tag.ptr");
    auto *tag_val = builder.CreateLoad(llvm::Type::getInt8Ty(context),
                                        tag_gep, "sw.union.tag");
    auto *i8_ty = llvm::Type::getInt8Ty(context);

    auto *default_bb = llvm::BasicBlock::Create(context, "sw.default");
    auto *sw = builder.CreateSwitch(tag_val, default_bb, node.arms.size());

    // Determine the subject variable name for narrowing.
    std::string subject_var;
    if (auto *id = std::get_if<IdentifierNode>(&node.subject->data))
      subject_var = std::string(id->name);

    for (size_t i = 0; i < node.arms.size(); ++i) {
      auto &arm = node.arms[i];
      auto *case_bb = llvm::BasicBlock::Create(context,
          "sw.case." + std::to_string(i), func);

      // Multi-pattern arms route every pattern to the same case block.
      // Narrowing the subject only makes sense with a single pattern.
      TypePtr pattern_sem;
      for (size_t pi = 0; pi < arm.patterns.size(); ++pi) {
        auto p_sem = semantic_type(*arm.patterns[pi]);
        if (pi == 0)
          pattern_sem = p_sem;
        int tag = -1;
        if (p_sem)
          tag = union_tag_for_type(p_sem, subject_sem);
        if (tag >= 0)
          sw->addCase(llvm::ConstantInt::get(i8_ty, tag), case_bb);
        else
          sw->addCase(llvm::ConstantInt::get(i8_ty, i), case_bb);
      }

      builder.SetInsertPoint(case_bb);

      llvm::AllocaInst *saved = nullptr;
      if (!subject_var.empty() && pattern_sem && arm.patterns.size() == 1) {
        auto local_it = locals.find(subject_var);
        if (local_it != locals.end()) {
          auto *extracted = emit_union_extract(union_ptr, pattern_sem,
                                                subject_sem);
          if (extracted) {
            auto *narrowed = create_entry_alloca(
                func, subject_var + ".case", llvm_type(pattern_sem));
            builder.CreateStore(extracted, narrowed);
            saved = local_it->second;
            locals[subject_var] = narrowed;
          }
        }
      }

      llvm::Value *body_val = nullptr;
      if (auto *block = std::get_if<BlockNode>(&arm.body->data)) {
        body_val = emit_block(*block);
      } else {
        body_val = emit_expr(*arm.body);
      }

      // Restore original local.
      if (saved)
        locals[subject_var] = saved;

      bool terminated = builder.GetInsertBlock()->getTerminator() != nullptr;
      if (!terminated)
        builder.CreateBr(merge_bb);
      case_results.push_back({body_val, builder.GetInsertBlock(), terminated});
    }

    // Default / else block.
    func->insert(func->end(), default_bb);
    builder.SetInsertPoint(default_bb);
    if (node.else_body) {
      llvm::Value *else_val = nullptr;
      if (auto *block = std::get_if<BlockNode>(&(*node.else_body)->data)) {
        else_val = emit_block(*block);
      } else {
        else_val = emit_expr(**node.else_body);
      }
      bool else_terminated =
          builder.GetInsertBlock()->getTerminator() != nullptr;
      if (!else_terminated)
        builder.CreateBr(merge_bb);
      case_results.push_back({else_val, builder.GetInsertBlock(),
                              else_terminated});
    } else {
      // Exhaustiveness is enforced by check_switch_expr — the default
      // is unreachable when all union alternatives are covered.  Adding
      // a null-valued case_result here would break the merge-PHI's
      // all_have_value check.
      builder.CreateUnreachable();
    }

  } else if (is_string) {
    // ── String matching: chained icmp + br ──────────────────────────
    auto *cmp_fn = module->getFunction("saga_string_compare");

    for (size_t i = 0; i < node.arms.size(); ++i) {
      auto &arm = node.arms[i];
      auto *case_bb = llvm::BasicBlock::Create(context,
          "sw.case." + std::to_string(i), func);
      auto *next_bb = llvm::BasicBlock::Create(context,
          "sw.next." + std::to_string(i));

      for (size_t pi = 0; pi < arm.patterns.size(); ++pi) {
        auto *pattern_val = emit_expr(*arm.patterns[pi]);
        auto *cmp = builder.CreateCall(cmp_fn, {subject_val, pattern_val}, "strcmp");
        auto *is_eq = builder.CreateICmpEQ(cmp,
            llvm::ConstantInt::get(i64_type, 0), "sw.eq");
        bool is_last = (pi + 1 == arm.patterns.size());
        auto *fail_bb = is_last
            ? next_bb
            : llvm::BasicBlock::Create(context,
                "sw.try." + std::to_string(i) + "." + std::to_string(pi), func);
        builder.CreateCondBr(is_eq, case_bb, fail_bb);
        if (!is_last)
          builder.SetInsertPoint(fail_bb);
      }

      // Emit the case body.
      builder.SetInsertPoint(case_bb);
      llvm::Value *body_val = nullptr;
      if (auto *block = std::get_if<BlockNode>(&arm.body->data)) {
        body_val = emit_block(*block);
      } else {
        body_val = emit_expr(*arm.body);
      }
      bool terminated = builder.GetInsertBlock()->getTerminator() != nullptr;
      if (!terminated)
        builder.CreateBr(merge_bb);
      case_results.push_back({body_val, builder.GetInsertBlock(), terminated});

      // Continue with the next comparison.
      func->insert(func->end(), next_bb);
      builder.SetInsertPoint(next_bb);
    }

    // Else clause or fall through to merge.
    llvm::Value *else_val = nullptr;
    bool else_terminated = false;
    llvm::BasicBlock *else_end_bb = nullptr;
    if (node.else_body) {
      if (auto *block = std::get_if<BlockNode>(&(*node.else_body)->data)) {
        else_val = emit_block(*block);
      } else {
        else_val = emit_expr(**node.else_body);
      }
      else_terminated = builder.GetInsertBlock()->getTerminator() != nullptr;
      else_end_bb = builder.GetInsertBlock();
      if (!else_terminated)
        builder.CreateBr(merge_bb);
    } else {
      builder.CreateBr(merge_bb);
    }
    if (!else_end_bb)
      else_end_bb = builder.GetInsertBlock();
    case_results.push_back({else_val, else_end_bb, else_terminated});

  } else {
    // ── Integer/Enum/Bool matching: LLVM switch instruction ─────────
    // Create a default block for the else clause.
    auto *default_bb = llvm::BasicBlock::Create(context, "sw.default");

    // Count the number of case arms for the switch.
    auto *sw = builder.CreateSwitch(subject_val, default_bb,
                                    node.arms.size());

    // Emit each case arm.
    for (size_t i = 0; i < node.arms.size(); ++i) {
      auto &arm = node.arms[i];
      auto *case_bb = llvm::BasicBlock::Create(context,
          "sw.case." + std::to_string(i), func);

      for (auto &pat : arm.patterns) {
        auto *pattern_val = emit_expr(*pat);
        if (auto *ci = llvm::dyn_cast_or_null<llvm::ConstantInt>(pattern_val)) {
          if (subject_val->getType()->isIntegerTy(1) &&
              ci->getType()->isIntegerTy(64)) {
            sw->addCase(llvm::ConstantInt::get(
                llvm::Type::getInt1Ty(context),
                ci->getZExtValue() & 1), case_bb);
          } else if (ci->getType() == subject_val->getType()) {
            sw->addCase(ci, case_bb);
          } else {
            auto *cast = llvm::ConstantInt::get(
                llvm::cast<llvm::IntegerType>(subject_val->getType()),
                ci->getSExtValue());
            sw->addCase(cast, case_bb);
          }
        } else {
          // Non-constant pattern — keep the block reachable with a
          // synthetic case so it isn't orphaned.  Well-formed programs
          // shouldn't hit this path.
          sw->addCase(llvm::ConstantInt::get(
              llvm::cast<llvm::IntegerType>(subject_val->getType()), i),
              case_bb);
        }
      }

      // Emit the case body.
      builder.SetInsertPoint(case_bb);
      llvm::Value *body_val = nullptr;
      if (auto *block = std::get_if<BlockNode>(&arm.body->data)) {
        body_val = emit_block(*block);
      } else {
        body_val = emit_expr(*arm.body);
      }
      bool terminated = builder.GetInsertBlock()->getTerminator() != nullptr;
      if (!terminated)
        builder.CreateBr(merge_bb);
      case_results.push_back({body_val, builder.GetInsertBlock(), terminated});
    }

    // Default / else block.
    func->insert(func->end(), default_bb);
    builder.SetInsertPoint(default_bb);
    llvm::Value *else_val = nullptr;
    bool else_terminated = false;
    if (node.else_body) {
      if (auto *block = std::get_if<BlockNode>(&(*node.else_body)->data)) {
        else_val = emit_block(*block);
      } else {
        else_val = emit_expr(**node.else_body);
      }
      else_terminated = builder.GetInsertBlock()->getTerminator() != nullptr;
    }
    if (!else_terminated)
      builder.CreateBr(merge_bb);
    case_results.push_back({else_val, builder.GetInsertBlock(), else_terminated});
  }

  // ── Merge block with optional PHI ──────────────────────────────────
  func->insert(func->end(), merge_bb);
  builder.SetInsertPoint(merge_bb);

  // Check if all results are the same non-void type for PHI.
  llvm::Type *phi_type = nullptr;
  bool all_have_value = true;
  bool any_reaches_merge = false;
  for (auto &cr : case_results) {
    if (!cr.terminated)
      any_reaches_merge = true;
    if (!cr.value || cr.terminated) {
      all_have_value = false;
    } else if (!phi_type) {
      phi_type = cr.value->getType();
    } else if (cr.value->getType() != phi_type) {
      all_have_value = false;
    }
  }

  if (!any_reaches_merge) {
    // All branches terminated (returned) — merge is unreachable.
    builder.CreateUnreachable();
    return nullptr;
  }

  if (all_have_value && phi_type && !phi_type->isVoidTy()) {
    unsigned incoming = 0;
    for (auto &cr : case_results)
      if (!cr.terminated)
        incoming++;
    auto *phi = builder.CreatePHI(phi_type, incoming, "sw.val");
    for (auto &cr : case_results) {
      if (!cr.terminated && cr.value)
        phi->addIncoming(cr.value, cr.block);
    }
    return phi;
  }

  return nullptr;
}

} // namespace saga

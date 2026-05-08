// Copyright 2026 Rob Thornton
// SPDX-License-Identifier: MIT

#include "ir/codegen.hpp"

#include <llvm/IR/Constants.h>

#include <algorithm>

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

llvm::Value *CodeGen::emit_if_expr(const IfExprNode &node) {
  auto *cond = emit_expr(*node.condition);
  if (!cond)
    return nullptr;

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

  // ── Detect type-matching pattern for narrowing ─────────────────────
  // If condition is `value == TypeName`, extract the narrowed value.
  std::string narrowed_var_name;
  llvm::AllocaInst *saved_alloca = nullptr;
  if (auto *binop = std::get_if<BinaryExprNode>(&node.condition->data)) {
    if (binop->op == Token::Kind::Equal) {
      if (auto *lhs_id = std::get_if<IdentifierNode>(&binop->lhs->data)) {
        auto lhs_sem = semantic_type(*binop->lhs);
        if (lhs_sem && lhs_sem->kind == TypeKind::Union) {
          auto rhs_sem = semantic_type(*binop->rhs);
          if (rhs_sem) {
            narrowed_var_name = std::string(lhs_id->name);
          }
        }
      }
    }
  }

  // ── Then block ─────────────────────────────────────────────────────
  builder.SetInsertPoint(then_bb);

  // If type-matching, narrow the variable by extracting from the union.
  if (!narrowed_var_name.empty()) {
    auto local_it = locals.find(narrowed_var_name);
    if (local_it != locals.end()) {
      auto lhs_sem = semantic_type(*std::get<BinaryExprNode>(
          node.condition->data).lhs);
      auto rhs_sem = semantic_type(*std::get<BinaryExprNode>(
          node.condition->data).rhs);
      if (lhs_sem && rhs_sem) {
        auto *union_ptr = local_it->second;
        auto *extracted = emit_union_extract(union_ptr, rhs_sem, lhs_sem);
        if (extracted) {
          auto *ll_type = llvm_type(rhs_sem);
          auto *narrowed_alloca = create_entry_alloca(
              func, narrowed_var_name + ".narrowed", ll_type);
          builder.CreateStore(extracted, narrowed_alloca);
          // Temporarily replace the local.
          saved_alloca = local_it->second;
          locals[narrowed_var_name] = narrowed_alloca;
        }
      }
    }
  }

  auto &then_block = std::get<BlockNode>(node.then_block->data);
  auto *then_val = emit_block(then_block);

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
    auto &else_block = std::get<BlockNode>((*node.else_block)->data);
    else_val = emit_block(else_block);
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

  // Build a PHI node if both branches produce a value of the same type.
  if (then_val && else_val &&
      then_val->getType() == else_val->getType() &&
      !then_terminated && !else_terminated) {
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
    case_results.push_back({else_val, builder.GetInsertBlock(),
                            else_terminated});

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


// ===========================================================================
// Array literals
// ===========================================================================

llvm::Value *CodeGen::emit_array_literal(const ArrayLiteralNode &node) {
  // Determine element size from the semantic type.
  auto sem = semantic_type(
      *reinterpret_cast<const Node *>(&node)); // hack: node is embedded
  // Try to get element type from the first element.
  int64_t elem_size = 8; // default to i64 size
  llvm::Type *elem_ll_type = i64_type;

  if (!node.elements.empty()) {
    auto first_sem = semantic_type(*node.elements[0]);
    if (first_sem) {
      elem_ll_type = llvm_type(first_sem);
      if (elem_ll_type->isStructTy())
        elem_size = module->getDataLayout().getTypeAllocSize(elem_ll_type);
      else if (elem_ll_type->isIntegerTy(1))
        elem_size = 1;
      else
        elem_size = 8;
    }
  }

  // Create the array: saga_array_new(elem_size, initial_cap)
  auto *new_fn = module->getFunction("saga_array_new");
  auto *arr = builder.CreateCall(
      new_fn,
      {llvm::ConstantInt::get(i64_type, elem_size),
       llvm::ConstantInt::get(i64_type,
                              std::max((int64_t)node.elements.size(), (int64_t)4))},
      "arr");

  // Push each element.
  auto *push_fn = module->getFunction("saga_array_push");
  auto *func = builder.GetInsertBlock()->getParent();

  for (auto &elem_node : node.elements) {
    auto *val = emit_expr(*elem_node);
    if (!val)
      continue;

    // saga_array_push takes a void* to the element and memcpy's
    // elem_size bytes from it.  For struct elements we pass the alloca
    // pointer directly; for SSA values we spill to a temp first.
    if (elem_ll_type->isStructTy()) {
      llvm::Value *src = val;
      if (val->getType()->isStructTy()) {
        auto *tmp = create_entry_alloca(func, "elem.tmp", elem_ll_type);
        builder.CreateStore(val, tmp);
        src = tmp;
      }
      builder.CreateCall(push_fn, {arr, src});
    } else {
      auto *tmp = create_entry_alloca(func, "elem.tmp", val->getType());
      builder.CreateStore(val, tmp);
      builder.CreateCall(push_fn, {arr, tmp});
    }
  }

  return arr;
}

// ===========================================================================
// Map literals
// ===========================================================================

llvm::Value *CodeGen::emit_map_literal(const MapLiteralNode &node) {
  // Determine key/value sizes from semantic types.
  int64_t key_size = 8;  // default to i64 size
  int64_t val_size = 8;
  llvm::Type *key_ll_type = i64_type;
  llvm::Type *val_ll_type = i64_type;
  TypePtr key_sem;

  // Get semantic type of the map literal node itself.
  // We look through the entries to determine types.
  if (!node.entries.empty()) {
    key_sem = semantic_type(*node.entries[0].key);
    auto val_sem = semantic_type(*node.entries[0].value);
    if (key_sem) {
      key_ll_type = llvm_type(key_sem);
      if (key_ll_type->isStructTy())
        key_size = module->getDataLayout().getTypeAllocSize(key_ll_type);
      else if (key_ll_type->isIntegerTy(1))
        key_size = 1;
      else
        key_size = 8;
    }
    if (val_sem) {
      val_ll_type = llvm_type(val_sem);
      if (val_ll_type->isStructTy())
        val_size = module->getDataLayout().getTypeAllocSize(val_ll_type);
      else if (val_ll_type->isIntegerTy(1))
        val_size = 1;
      else
        val_size = 8;
    }
  }

  int64_t key_kind_tag =
      static_cast<int64_t>(CodeGen::key_kind_for(key_sem));
  llvm::Constant *ops_ptr = get_or_emit_key_ops(key_sem);

  // Create the map: saga_map_new(key_size, val_size, key_kind, ops)
  auto *new_fn = module->getFunction("saga_map_new");
  auto *map = builder.CreateCall(
      new_fn,
      {llvm::ConstantInt::get(i64_type, key_size),
       llvm::ConstantInt::get(i64_type, val_size),
       llvm::ConstantInt::get(i64_type, key_kind_tag),
       ops_ptr},
      "map");

  // Insert each entry.
  auto *set_fn = module->getFunction("saga_map_set");
  auto *func = builder.GetInsertBlock()->getParent();

  for (auto &entry : node.entries) {
    auto *key_val = emit_expr(*entry.key);
    auto *val_val = emit_expr(*entry.value);
    if (!key_val || !val_val)
      continue;

    // Key spill (struct keys not supported here yet; default scalar path).
    auto *key_tmp = create_entry_alloca(func, "map.key.tmp", key_val->getType());
    builder.CreateStore(key_val, key_tmp);

    // Value: for struct values, pass the struct alloca pointer directly
    // so the runtime memcpy's val_size bytes of struct contents.
    llvm::Value *val_ptr = nullptr;
    if (val_ll_type->isStructTy()) {
      if (val_val->getType()->isStructTy()) {
        auto *tmp = create_entry_alloca(func, "map.val.tmp", val_ll_type);
        builder.CreateStore(val_val, tmp);
        val_ptr = tmp;
      } else {
        val_ptr = val_val; // already a pointer to a struct alloca
      }
    } else {
      auto *tmp = create_entry_alloca(func, "map.val.tmp", val_val->getType());
      builder.CreateStore(val_val, tmp);
      val_ptr = tmp;
    }

    builder.CreateCall(set_fn, {map, key_tmp, val_ptr});
  }

  return map;
}

// ===========================================================================
// Index expressions
// ===========================================================================

llvm::Value *
CodeGen::wrap_indexed_lookup_in_error_union(llvm::Value *elem_ptr,
                                            llvm::Type *elem_ll,
                                            const TypePtr &val_type) {
  auto result_union =
      make_union_type({val_type, analyzer.builtins.error_iface});
  auto *union_st = get_union_llvm_type(result_union);
  if (!union_st)
    return nullptr;

  auto *func = builder.GetInsertBlock()->getParent();
  auto *ptr_ty = llvm::PointerType::getUnqual(context);
  auto *is_null = builder.CreateICmpEQ(
      elem_ptr, llvm::ConstantPointerNull::get(ptr_ty), "idx.is_null");

  auto *null_bb = llvm::BasicBlock::Create(context, "idx.null", func);
  auto *ok_bb = llvm::BasicBlock::Create(context, "idx.ok", func);
  auto *merge_bb = llvm::BasicBlock::Create(context, "idx.merge", func);

  builder.CreateCondBr(is_null, null_bb, ok_bb);

  builder.SetInsertPoint(null_bb);
  auto missing_sem = analyzer.builtins.missing_type;
  auto *missing_val =
      llvm::Constant::getNullValue(llvm::StructType::get(context));
  auto *err_wrapped =
      emit_union_wrap(missing_val, missing_sem, result_union);
  auto *null_end_bb = builder.GetInsertBlock();
  builder.CreateBr(merge_bb);

  builder.SetInsertPoint(ok_bb);
  auto *loaded = builder.CreateLoad(elem_ll, elem_ptr, "elem");
  auto *ok_wrapped = emit_union_wrap(loaded, val_type, result_union);
  auto *ok_end_bb = builder.GetInsertBlock();
  builder.CreateBr(merge_bb);

  builder.SetInsertPoint(merge_bb);
  auto *phi = builder.CreatePHI(ptr_ty, 2, "idx.union");
  phi->addIncoming(err_wrapped, null_end_bb);
  phi->addIncoming(ok_wrapped, ok_end_bb);
  return phi;
}

llvm::Value *CodeGen::emit_index_expr(const IndexExprNode &node) {
  auto *obj = emit_expr(*node.object);
  if (!obj)
    return nullptr;

  auto obj_sem = semantic_type(*node.object);

  if (obj_sem && obj_sem->kind == TypeKind::Array) {
    auto *idx = emit_expr(*node.index);
    if (!idx)
      return nullptr;

    auto *at_fn = module->getFunction("saga_array_at");
    auto *elem_ptr = builder.CreateCall(at_fn, {obj, idx}, "at");

    auto &arr_info = std::get<ArrayTypeInfo>(obj_sem->detail);
    auto *elem_ll = llvm_type(arr_info.element);

    return wrap_indexed_lookup_in_error_union(elem_ptr, elem_ll,
                                              arr_info.element);
  }

  if (obj_sem && obj_sem->kind == TypeKind::Map) {
    auto *idx = emit_expr(*node.index);
    if (!idx)
      return nullptr;

    auto &map_info = std::get<MapTypeInfo>(obj_sem->detail);

    auto *func = builder.GetInsertBlock()->getParent();

    auto *key_tmp = create_entry_alloca(func, "map.idx.key", idx->getType());
    builder.CreateStore(idx, key_tmp);

    auto *get_fn = module->getFunction("saga_map_get");
    auto *val_ptr = builder.CreateCall(get_fn, {obj, key_tmp}, "map.get");

    auto *val_ll = llvm_type(map_info.value);
    return wrap_indexed_lookup_in_error_union(val_ptr, val_ll, map_info.value);
  }

  // String indexing — deferred for now.
  return nullptr;
}

// ===========================================================================
// Or expression (error stripping)
// ===========================================================================

llvm::Value *CodeGen::emit_or_expr(const OrExprNode &node) {
  // Emit the expression that may produce a union with Error.
  auto *expr_val = emit_expr(*node.expr);
  if (!expr_val)
    return nullptr;

  auto expr_sem = semantic_type(*node.expr);
  if (!expr_sem)
    return expr_val;

  // If the expression is not a union, just return the value.
  if (expr_sem->kind != TypeKind::Union)
    return expr_val;

  // Check if this is an impure union (contains Error).
  if (!is_impure_union(expr_sem))
    return expr_val;

  auto *union_st = get_union_llvm_type(expr_sem);
  if (!union_st)
    return expr_val;

  auto *func = builder.GetInsertBlock()->getParent();

  // The expr_val should be an alloca (pointer to the union struct).
  // If it's not already a pointer to the union, we need to handle that.
  llvm::Value *union_ptr = expr_val;

  // If union_ptr is a loaded value (struct type, not pointer), store it.
  if (!union_ptr->getType()->isPointerTy() ||
      (llvm::isa<llvm::LoadInst>(union_ptr))) {
    auto *tmp = create_entry_alloca(func, "or.union", union_st);
    builder.CreateStore(expr_val, tmp);
    union_ptr = tmp;
  }

  // Load the tag.
  auto *tag_gep = builder.CreateStructGEP(union_st, union_ptr, 0, "or.tag");
  auto *tag = builder.CreateLoad(llvm::Type::getInt8Ty(context), tag_gep,
                                  "or.tag.val");

  // Find which tag values correspond to Error types.
  auto &info = std::get<UnionTypeInfo>(expr_sem->detail);
  std::vector<int> error_tags;
  std::vector<int> non_error_tags;
  for (size_t i = 0; i < info.alternatives.size(); ++i) {
    auto &alt = info.alternatives[i];
    if (alt->kind == TypeKind::Interface) {
      auto &iface = std::get<InterfaceTypeInfo>(alt->detail);
      if (iface.name == "Error") {
        error_tags.push_back(static_cast<int>(i));
        continue;
      }
    }
    non_error_tags.push_back(static_cast<int>(i));
  }

  // Create basic blocks.
  auto *ok_bb = llvm::BasicBlock::Create(context, "or.ok", func);
  auto *err_bb = llvm::BasicBlock::Create(context, "or.err");
  auto *merge_bb = llvm::BasicBlock::Create(context, "or.merge");

  // Branch based on whether the tag is an error tag.
  // If there's only one error tag, simple comparison.
  // For multiple error tags, we'd need an or-chain, but typically there's
  // just one Error interface in the union.
  if (error_tags.size() == 1) {
    auto *is_err = builder.CreateICmpEQ(
        tag,
        llvm::ConstantInt::get(llvm::Type::getInt8Ty(context), error_tags[0]),
        "or.is_err");
    builder.CreateCondBr(is_err, err_bb, ok_bb);
  } else {
    // Multiple error tags — build an OR chain.
    llvm::Value *is_err = llvm::ConstantInt::get(
        llvm::Type::getInt1Ty(context), 0);
    for (int et : error_tags) {
      auto *cmp = builder.CreateICmpEQ(
          tag,
          llvm::ConstantInt::get(llvm::Type::getInt8Ty(context), et),
          "or.cmp");
      is_err = builder.CreateOr(is_err, cmp, "or.any_err");
    }
    builder.CreateCondBr(is_err, err_bb, ok_bb);
  }

  // ── OK block: extract the non-error value ──────────────────────────
  builder.SetInsertPoint(ok_bb);

  // Determine the purified result type.
  TypePtr purified = strip_error_from_union(expr_sem);
  llvm::Value *ok_val = nullptr;

  if (purified && purified->kind == TypeKind::Union) {
    // Multiple non-error alternatives remain — result is still a union.
    // Re-wrap into the purified union type.
    auto *purified_st = get_union_llvm_type(purified);
    auto *purified_alloca = create_entry_alloca(func, "or.purified",
                                                 purified_st);
    builder.CreateStore(llvm::Constant::getNullValue(purified_st),
                        purified_alloca);

    // We need to remap the tag. The original tag corresponds to the position
    // in the full union; we need the position in the purified union.
    auto &pur_info = std::get<UnionTypeInfo>(purified->detail);
    auto *i8_ty = llvm::Type::getInt8Ty(context);

    // Build a switch to remap tags and copy the payload.
    auto *remap_default = llvm::BasicBlock::Create(context, "or.remap.def");
    auto *remap_merge = llvm::BasicBlock::Create(context, "or.remap.merge");
    auto *sw = builder.CreateSwitch(tag, remap_default, non_error_tags.size());

    std::vector<std::pair<llvm::Value *, llvm::BasicBlock *>> phi_entries;

    for (int orig_tag : non_error_tags) {
      auto *case_bb = llvm::BasicBlock::Create(
          context, "or.remap." + std::to_string(orig_tag), func);
      sw->addCase(llvm::ConstantInt::get(i8_ty, orig_tag), case_bb);

      builder.SetInsertPoint(case_bb);

      // Find the new tag index in the purified union.
      int new_tag = -1;
      for (size_t pi = 0; pi < pur_info.alternatives.size(); ++pi) {
        if (types_equal(pur_info.alternatives[pi],
                        info.alternatives[orig_tag])) {
          new_tag = static_cast<int>(pi);
          break;
        }
      }
      if (new_tag < 0) new_tag = 0;

      // Set the new tag.
      auto *ptag_gep = builder.CreateStructGEP(purified_st, purified_alloca,
                                                 0, "pur.tag");
      builder.CreateStore(llvm::ConstantInt::get(i8_ty, new_tag), ptag_gep);

      // Copy the payload bytes.
      auto *src_payload = builder.CreateStructGEP(union_st, union_ptr, 1,
                                                   "src.payload");
      auto *dst_payload = builder.CreateStructGEP(purified_st,
                                                   purified_alloca, 1,
                                                   "dst.payload");
      uint64_t pay_sz = union_payload_size(purified);
      builder.CreateMemCpy(dst_payload, llvm::Align(1),
                           src_payload, llvm::Align(1), pay_sz);

      builder.CreateBr(remap_merge);
      phi_entries.push_back({purified_alloca, builder.GetInsertBlock()});
    }

    func->insert(func->end(), remap_default);
    builder.SetInsertPoint(remap_default);
    builder.CreateBr(remap_merge);

    func->insert(func->end(), remap_merge);
    builder.SetInsertPoint(remap_merge);

    // Load the purified union struct for the PHI.
    ok_val = builder.CreateLoad(purified_st, purified_alloca, "or.ok.val");
  } else if (purified) {
    // Single non-error alternative — extract it directly.
    ok_val = emit_union_extract(union_ptr, purified, expr_sem);
  }

  if (!ok_val)
    ok_val = llvm::Constant::getNullValue(
        purified ? llvm_type(purified) : i64_type);

  builder.CreateBr(merge_bb);
  auto *ok_end_bb = builder.GetInsertBlock();

  // ── Error block: emit fallback ─────────────────────────────────────
  func->insert(func->end(), err_bb);
  builder.SetInsertPoint(err_bb);

  // Bind the pipe variable to the Error payload extracted from the
  // union.  The payload first 8 bytes hold the interface fat pointer
  // produced by whichever path produced the Error (e.g. Task.Wait's
  // saga_error_from_trap).
  if (node.pipe) {
    std::string pipe_name(node.pipe->name);
    auto *ptr_type = llvm::PointerType::getUnqual(context);
    auto *err_alloca = create_entry_alloca(func, pipe_name, ptr_type);
    auto *payload_gep = builder.CreateStructGEP(union_st, union_ptr, 1,
                                                 "err.payload.gep");
    auto *err_val = builder.CreateLoad(ptr_type, payload_gep,
                                        "err.payload.val");
    builder.CreateStore(err_val, err_alloca);
    locals[pipe_name] = err_alloca;
  }

  auto &fallback_block = std::get<BlockNode>(node.fallback->data);
  auto *fallback_val = emit_block(fallback_block);

  // The fallback value must match the purified type.
  if (!fallback_val && ok_val)
    fallback_val = llvm::Constant::getNullValue(ok_val->getType());

  // Coerce fallback to match ok_val type if needed.
  if (fallback_val && ok_val &&
      fallback_val->getType() != ok_val->getType()) {
    // If the result is a union but fallback is a concrete value, wrap it.
    if (purified && purified->kind == TypeKind::Union && fallback_val) {
      // Try to find the semantic type of the fallback.
      auto fb_sem = semantic_type(*node.fallback);
      if (fb_sem && fb_sem->kind != TypeKind::Union) {
        auto *wrapped = emit_union_wrap(fallback_val, fb_sem, purified);
        if (wrapped) {
          auto *purified_st = get_union_llvm_type(purified);
          fallback_val = builder.CreateLoad(purified_st, wrapped,
                                             "or.fb.union");
        }
      }
    } else {
      // Type mismatch — use null of ok type.
      fallback_val = llvm::Constant::getNullValue(ok_val->getType());
    }
  }

  bool err_terminated = builder.GetInsertBlock()->getTerminator() != nullptr;
  if (!err_terminated)
    builder.CreateBr(merge_bb);
  auto *err_end_bb = builder.GetInsertBlock();

  // Clean up pipe variable.
  if (node.pipe) {
    locals.erase(std::string(node.pipe->name));
  }

  // ── Merge block ────────────────────────────────────────────────────
  func->insert(func->end(), merge_bb);
  builder.SetInsertPoint(merge_bb);

  if (ok_val && fallback_val &&
      ok_val->getType() == fallback_val->getType() && !err_terminated) {
    auto *phi = builder.CreatePHI(ok_val->getType(), 2, "or.result");
    phi->addIncoming(ok_val, ok_end_bb);
    phi->addIncoming(fallback_val, err_end_bb);
    return phi;
  }

  return ok_val;
}

// ===========================================================================
// Struct literals
// ===========================================================================

llvm::Value *CodeGen::emit_struct_literal(const StructLiteralNode &node,
                                          const Node &parent) {
  // Prefer the analyzer's recorded type for the literal expression — for
  // a generic struct literal like `Box{value: Point{...}}` the parent
  // node carries the instantiation `Box<Point>` (with substituted fields
  // and concrete type_args), whereas `node.type_expr` is just the bare
  // template identifier.  Fall back to the type-expr type for older
  // call sites or non-generic structs.
  auto sem = semantic_type(parent);
  if (!sem || sem->kind != TypeKind::Struct)
    sem = semantic_type(*node.type_expr);
  if (!sem || sem->kind != TypeKind::Struct)
    return nullptr;

  auto &info = std::get<StructTypeInfo>(sem->detail);
  // Materialize the per-instantiation LLVM struct on first use; for
  // non-generic structs this is a cache hit on the type registered by
  // emit_struct_decl/materialize_import.
  llvm_type(sem);
  std::string skey = struct_cache_key(info);
  auto st_it = struct_types.find(skey);
  if (st_it == struct_types.end())
    return nullptr;

  auto *st = st_it->second;

  // Allocate the struct on the stack.
  auto *func = builder.GetInsertBlock()->getParent();
  auto *alloca = create_entry_alloca(func, info.name + ".lit", st);

  // Zero-initialize all fields.
  builder.CreateStore(llvm::Constant::getNullValue(st), alloca);

  // Store each provided field value. For a literal that addresses a
  // promoted field (the field lives on an embedded struct), reuse
  // struct_field_gep — it already walks the embed layout and hands back
  // a pointer into the right inner slot.
  for (auto &fa : node.fields) {
    std::string fname(fa.name.name);

    auto *val = emit_expr(*fa.value);
    if (!val)
      continue;

    auto [gep, field_ll] = struct_field_gep(alloca, sem, fname);
    if (!gep)
      continue;

    // D1: aggregate fields are stored inline. If the rhs is a pointer to
    // a struct (e.g. from a nested struct literal), memcpy the bytes
    // rather than storing the pointer into the struct slot.
    if (field_ll && field_ll->isStructTy() && val->getType()->isPointerTy()) {
      auto &dl = module->getDataLayout();
      uint64_t sz = dl.getTypeAllocSize(field_ll);
      llvm::Align al = dl.getABITypeAlign(field_ll);
      builder.CreateMemCpy(gep, al, val, al, sz);
    } else {
      builder.CreateStore(val, gep);
    }
  }

  // Return a pointer to the struct alloca.
  return alloca;
}

// ===========================================================================
// Selector (field access)
// ===========================================================================

std::pair<llvm::Value *, llvm::Type *>
CodeGen::struct_field_gep(llvm::Value *struct_ptr,
                          const TypePtr &struct_sem_type,
                          const std::string &field_name) {
  if (!struct_sem_type || struct_sem_type->kind != TypeKind::Struct)
    return {nullptr, nullptr};

  auto &info = std::get<StructTypeInfo>(struct_sem_type->detail);
  std::string skey = struct_cache_key(info);
  auto st_it = struct_types.find(skey);
  if (st_it == struct_types.end())
    return {nullptr, nullptr};

  auto *st = st_it->second;
  auto &fields = struct_fields[skey];

  // Direct field lookup. We restrict to info.fields.size() so that the
  // synthetic `__embed_<Name>` slots appended after the own fields are
  // not addressable by name from user code — they are reachable only via
  // promoted-field access (handled below).
  for (size_t i = 0; i < info.fields.size() && i < fields.size(); ++i) {
    if (fields[i] == field_name) {
      auto *gep = builder.CreateStructGEP(st, struct_ptr, i, field_name);
      // Prefer the semantic field type's LLVM lowering so generic
      // instantiations (e.g. Box<Int>) read/write at the right element
      // type even when the underlying LLVM struct was emitted with a
      // ptr-typed slot for the unsubstituted template field. Sizes must
      // match the slot for this to be safe; aggregate type arguments
      // wider than a pointer are tracked as P8 tech debt.
      llvm::Type *field_ll = st->getElementType(i);
      if (info.fields[i].type) {
        if (auto *sem_ll = llvm_type(info.fields[i].type))
          field_ll = sem_ll;
      }
      return {gep, field_ll};
    }
  }

  // Promoted-field access: the field lives on one of the embedded structs.
  // Walk each embed in declaration order and recurse into its layout.
  // Layout invariant: embed slots are appended to `fields` after the
  // owner's own fields, in the same order as `info.embeds`.
  for (size_t ei = 0; ei < info.embeds.size(); ++ei) {
    auto &embed = info.embeds[ei];
    if (!embed || embed->kind != TypeKind::Struct) continue;
    size_t slot_idx = info.fields.size() + ei;
    if (slot_idx >= fields.size()) break;

    auto &einfo = std::get<StructTypeInfo>(embed->detail);
    auto *slot_gep = builder.CreateStructGEP(st, struct_ptr, slot_idx,
                                             embed_slot_name(einfo));
    auto inner = struct_field_gep(slot_gep, embed, field_name);
    if (inner.first) return inner;
  }

  return {nullptr, nullptr};
}


} // namespace saga

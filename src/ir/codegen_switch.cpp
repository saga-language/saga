// Copyright 2026 Rob Thornton
// SPDX-License-Identifier: MIT

// The switch expression, dispatched by its subject: a union on its tag, a
// string by comparison, anything else with an LLVM switch. Every arm ends in
// the one join the switch's value is taken from (codegen_join.cpp).

#include "ir/codegen.hpp"

#include <llvm/IR/Constants.h>

namespace saga {

llvm::Value *CodeGen::emit_switch_expr(const SwitchExprNode &node,
                                       const Node &parent) {
  if (node.init)
    emit_expr(**node.init);
  auto *subject = emit_expr(*node.subject);
  if (!subject)
    return nullptr;

  auto join = open_join("sw.merge", semantic_type(parent));
  auto subject_sem = semantic_type(*node.subject);
  if (subject_sem && subject_sem->kind == TypeKind::Union)
    emit_type_switch(node, subject, subject_sem, join);
  else if (subject_sem && subject_sem->kind == TypeKind::String)
    emit_string_switch(node, subject, join);
  else
    emit_value_switch(node, subject, join);
  return finish_join(join, "sw.val");
}

// A missing else is an arm with no value, which yields the zero value.
void CodeGen::emit_switch_arm(BranchJoin &join, const Node *body) {
  llvm::Value *val = nullptr;
  TypePtr sem;
  if (body) {
    auto *block = std::get_if<BlockNode>(&body->data);
    val = block ? emit_block(*block) : emit_expr(*body);
    sem = body_result_type(*body);
  }
  close_branch(join, val, sem);
}

// The analyzer holds a type switch with no else to covering every
// alternative, so its default is unreachable.
void CodeGen::emit_type_switch(const SwitchExprNode &node, llvm::Value *subject,
                               const TypePtr &subject_sem, BranchJoin &join) {
  auto *func = builder.GetInsertBlock()->getParent();
  auto *tag_gep = builder.CreateStructGEP(get_union_llvm_type(subject_sem),
                                          subject, 0, "sw.union.tag.ptr");
  auto *tag = builder.CreateLoad(llvm::Type::getInt8Ty(context), tag_gep,
                                 "sw.union.tag");
  auto *default_bb = llvm::BasicBlock::Create(context, "sw.default");
  auto *sw = builder.CreateSwitch(tag, default_bb, node.arms.size());

  for (size_t i = 0; i < node.arms.size(); ++i) {
    auto &arm = node.arms[i];
    auto *case_bb = llvm::BasicBlock::Create(
        context, "sw.case." + std::to_string(i), func);
    add_type_cases(sw, arm, subject_sem, i, case_bb);
    builder.SetInsertPoint(case_bb);
    auto narrowing = arm_narrowing(node, arm, subject_sem);
    auto *displaced = narrowing ? narrow_local(narrowing->name,
                                               narrowing->from, narrowing->to)
                                : nullptr;
    emit_switch_arm(join, arm.body.get());
    if (displaced)
      locals[narrowing->name] = displaced;
  }

  start_block(default_bb);
  if (node.else_body)
    emit_switch_arm(join, node.else_body->get());
  else
    builder.CreateUnreachable();
}

void CodeGen::add_type_cases(llvm::SwitchInst *sw, const CaseArmNode &arm,
                             const TypePtr &subject_sem, size_t arm_index,
                             llvm::BasicBlock *case_bb) {
  auto *i8_ty = llvm::Type::getInt8Ty(context);
  for (auto &pat : arm.patterns) {
    auto p_sem = semantic_type(*pat);
    int tag = p_sem ? union_tag_for_type(p_sem, subject_sem) : -1;
    sw->addCase(llvm::ConstantInt::get(i8_ty, tag >= 0 ? tag : arm_index),
                case_bb);
  }
}

// Narrowing a subject matched by several patterns would need their union,
// which nothing builds, so only a single-pattern arm narrows.
std::optional<CodeGen::Narrowing>
CodeGen::arm_narrowing(const SwitchExprNode &node, const CaseArmNode &arm,
                       const TypePtr &subject_sem) {
  auto *id = std::get_if<IdentifierNode>(&node.subject->data);
  if (!id || arm.patterns.size() != 1)
    return std::nullopt;
  auto to = semantic_type(*arm.patterns[0]);
  if (!to)
    return std::nullopt;
  return Narrowing{std::string(id->name), subject_sem, to};
}

void CodeGen::emit_string_switch(const SwitchExprNode &node,
                                 llvm::Value *subject, BranchJoin &join) {
  auto *func = builder.GetInsertBlock()->getParent();
  for (size_t i = 0; i < node.arms.size(); ++i) {
    auto *case_bb = llvm::BasicBlock::Create(
        context, "sw.case." + std::to_string(i), func);
    auto *next_bb =
        llvm::BasicBlock::Create(context, "sw.next." + std::to_string(i));
    branch_on_string_patterns(node.arms[i], subject, i, case_bb, next_bb);
    builder.SetInsertPoint(case_bb);
    emit_switch_arm(join, node.arms[i].body.get());
    start_block(next_bb);
  }
  emit_switch_arm(join, node.else_body ? node.else_body->get() : nullptr);
}

void CodeGen::branch_on_string_patterns(const CaseArmNode &arm,
                                        llvm::Value *subject, size_t arm_index,
                                        llvm::BasicBlock *match,
                                        llvm::BasicBlock *miss) {
  auto *cmp_fn = module->getFunction("saga_string_compare");
  auto *func = builder.GetInsertBlock()->getParent();
  for (size_t pi = 0; pi < arm.patterns.size(); ++pi) {
    auto *pattern = emit_expr(*arm.patterns[pi]);
    auto *cmp = builder.CreateCall(cmp_fn, {subject, pattern}, "strcmp");
    auto *is_eq = builder.CreateICmpEQ(
        cmp, llvm::ConstantInt::get(i64_type, 0), "sw.eq");
    bool is_last = pi + 1 == arm.patterns.size();
    auto *fail_bb = is_last ? miss
                            : llvm::BasicBlock::Create(
                                  context,
                                  "sw.try." + std::to_string(arm_index) + "." +
                                      std::to_string(pi),
                                  func);
    builder.CreateCondBr(is_eq, match, fail_bb);
    if (!is_last)
      builder.SetInsertPoint(fail_bb);
  }
}

void CodeGen::emit_value_switch(const SwitchExprNode &node,
                                llvm::Value *subject, BranchJoin &join) {
  auto *func = builder.GetInsertBlock()->getParent();
  auto *default_bb = llvm::BasicBlock::Create(context, "sw.default");
  auto *sw = builder.CreateSwitch(subject, default_bb, node.arms.size());
  for (size_t i = 0; i < node.arms.size(); ++i) {
    auto *case_bb = llvm::BasicBlock::Create(
        context, "sw.case." + std::to_string(i), func);
    for (auto &pat : node.arms[i].patterns)
      sw->addCase(case_constant(emit_expr(*pat), subject->getType(), i),
                  case_bb);
    builder.SetInsertPoint(case_bb);
    emit_switch_arm(join, node.arms[i].body.get());
  }
  start_block(default_bb);
  emit_switch_arm(join, node.else_body ? node.else_body->get() : nullptr);
}

// A pattern is matched at the subject's width: a bool subject is an i1 where
// an integer literal is an i64. A pattern that is not a constant keeps its
// block reachable through a synthetic case.
llvm::ConstantInt *CodeGen::case_constant(llvm::Value *pattern,
                                          llvm::Type *subject_ll,
                                          size_t arm_index) {
  auto *width = llvm::cast<llvm::IntegerType>(subject_ll);
  auto *ci = llvm::dyn_cast_or_null<llvm::ConstantInt>(pattern);
  if (!ci)
    return llvm::ConstantInt::get(width, arm_index);
  if (ci->getType() == subject_ll)
    return ci;
  if (subject_ll->isIntegerTy(1))
    return llvm::ConstantInt::get(width, ci->getZExtValue() & 1);
  return llvm::ConstantInt::get(width, ci->getSExtValue());
}

} // namespace saga

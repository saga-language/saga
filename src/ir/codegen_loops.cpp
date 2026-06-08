// Copyright 2026 Rob Thornton
// SPDX-License-Identifier: MIT

#include "ir/codegen.hpp"

#include <llvm/IR/Constants.h>

namespace saga {

namespace {

void tick_reduction(CodeGen &cg) {
  if (cg.current_actor)
    cg.builder.CreateCall(cg.module->getFunction("saga_reduction_tick"),
                          {cg.current_actor});
}

llvm::Value *to_bool(llvm::IRBuilder<> &b, llvm::Value *v) {
  if (!v)
    return nullptr;
  if (v->getType()->isIntegerTy(1))
    return v;
  return b.CreateICmpNE(v, llvm::Constant::getNullValue(v->getType()),
                        "tobool");
}

} // namespace

llvm::Value *CodeGen::emit_for_expr(const ForExprNode &node,
                                    const Node &parent) {
  auto *func = builder.GetInsertBlock()->getParent();

  auto for_sem = semantic_type(parent);

  // Accumulator setup: when the for-expression has `|acc|`, allocate a
  // local zero-initialised to the for-expression's recorded type, bind
  // it as `acc`, and load+return it after the loop exits.  Without
  // this, every for-expression returns null and `sum := for ... |acc|
  // {...}` would always be 0.
  llvm::AllocaInst *acc_alloca = nullptr;
  llvm::Type *acc_ll = nullptr;
  if (node.accumulator) {
    if (for_sem && for_sem->kind != TypeKind::Void) {
      acc_ll = llvm_type(for_sem);
      if (acc_ll && !acc_ll->isVoidTy()) {
        std::string acc_name(node.accumulator->name);
        acc_alloca = create_entry_alloca(func, acc_name, acc_ll);
        builder.CreateStore(llvm::Constant::getNullValue(acc_ll), acc_alloca);
        locals[acc_name] = acc_alloca;
      }
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
        if (alt && alt->kind == TypeKind::Interface &&
            std::get<InterfaceTypeInfo>(alt->detail).name == "Error") {
          err_tag = static_cast<int>(i);
        } else if (alt) {
          break_value_type = alt;
        }
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
  func->insert(func->end(), bbs.exit_bb);
  builder.SetInsertPoint(bbs.exit_bb);

  if (acc_alloca && acc_ll)
    return builder.CreateLoad(acc_ll, acc_alloca, "for.acc");
  if (break_result)
    return break_result;
  return nullptr;
}

void CodeGen::emit_for_infinite(const ForExprNode &node,
                                const ForLoopBlocks &bbs) {
  auto *func = builder.GetInsertBlock()->getParent();
  builder.CreateBr(bbs.body_bb);

  // cond_bb is the "next" trampoline that loops back to body.
  builder.SetInsertPoint(bbs.cond_bb);
  builder.CreateBr(bbs.body_bb);
  loop_stack.back().next_bb = bbs.cond_bb;

  func->insert(func->end(), bbs.body_bb);
  builder.SetInsertPoint(bbs.body_bb);
  tick_reduction(*this);
  auto &body_block = std::get<BlockNode>(node.body->data);
  emit_block(body_block);
  if (!builder.GetInsertBlock()->getTerminator())
    builder.CreateBr(bbs.cond_bb);
}

void CodeGen::emit_for_c_style(const ForExprNode &node,
                               const ForIterClauseNode &iter,
                               const ForLoopBlocks &bbs) {
  auto *func = builder.GetInsertBlock()->getParent();

  emit_expr(*iter.init);
  builder.CreateBr(bbs.cond_bb);

  builder.SetInsertPoint(bbs.cond_bb);
  auto *cond_val = to_bool(builder, emit_expr(*iter.condition));
  if (cond_val)
    builder.CreateCondBr(cond_val, bbs.body_bb, bbs.exit_bb);
  else
    builder.CreateBr(bbs.body_bb);

  func->insert(func->end(), bbs.body_bb);
  builder.SetInsertPoint(bbs.body_bb);
  tick_reduction(*this);
  auto &body_block = std::get<BlockNode>(node.body->data);
  emit_block(body_block);
  if (!builder.GetInsertBlock()->getTerminator())
    builder.CreateBr(bbs.update_bb);

  func->insert(func->end(), bbs.update_bb);
  builder.SetInsertPoint(bbs.update_bb);
  emit_expr(*iter.update);
  builder.CreateBr(bbs.cond_bb);
}

void CodeGen::emit_for_condition(const ForExprNode &node, const Node &mode,
                                 const ForLoopBlocks &bbs) {
  auto *func = builder.GetInsertBlock()->getParent();
  builder.CreateBr(bbs.cond_bb);

  builder.SetInsertPoint(bbs.cond_bb);
  auto *cond_val = to_bool(builder, emit_expr(mode));
  if (cond_val)
    builder.CreateCondBr(cond_val, bbs.body_bb, bbs.exit_bb);
  else
    builder.CreateBr(bbs.body_bb);

  func->insert(func->end(), bbs.body_bb);
  builder.SetInsertPoint(bbs.body_bb);
  tick_reduction(*this);
  auto &body_block = std::get<BlockNode>(node.body->data);
  emit_block(body_block);
  if (!builder.GetInsertBlock()->getTerminator())
    builder.CreateBr(bbs.update_bb);

  func->insert(func->end(), bbs.update_bb);
  builder.SetInsertPoint(bbs.update_bb);
  builder.CreateBr(bbs.cond_bb);
}

void CodeGen::emit_for_range(const ForExprNode &node,
                             const ForRangeClauseNode &range,
                             const ForLoopBlocks &bbs) {
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

void CodeGen::emit_for_range_array(const ForExprNode &node,
                                   const ForRangeClauseNode &range,
                                   llvm::Value *iterable,
                                   const TypePtr &iter_sem,
                                   const ForLoopBlocks &bbs) {
  auto *func = builder.GetInsertBlock()->getParent();
  auto &arr_info = std::get<ArrayTypeInfo>(iter_sem->detail);
  auto *elem_ll = llvm_type(arr_info.element);
  bool struct_elem = arr_info.element &&
                     arr_info.element->kind == TypeKind::Struct &&
                     elem_ll && elem_ll->isStructTy();

  auto *size_fn = module->getFunction("saga_array_size");
  auto *arr_len = builder.CreateCall(size_fn, {iterable}, "arr.len");

  auto *idx_alloca = create_entry_alloca(func, ".idx", i64_type);
  builder.CreateStore(llvm::ConstantInt::get(i64_type, 0), idx_alloca);

  llvm::AllocaInst *key_alloca = nullptr;
  llvm::AllocaInst *val_alloca = nullptr;
  if (range.vars.size() == 1) {
    val_alloca =
        create_entry_alloca(func, std::string(range.vars[0].name), elem_ll);
    locals[std::string(range.vars[0].name)] = val_alloca;
  } else if (range.vars.size() == 2) {
    key_alloca = create_entry_alloca(
        func, std::string(range.vars[0].name), i64_type);
    locals[std::string(range.vars[0].name)] = key_alloca;
    val_alloca = create_entry_alloca(
        func, std::string(range.vars[1].name), elem_ll);
    locals[std::string(range.vars[1].name)] = val_alloca;
  }

  builder.CreateBr(bbs.cond_bb);
  builder.SetInsertPoint(bbs.cond_bb);
  auto *cur_idx = builder.CreateLoad(i64_type, idx_alloca, "idx");
  auto *cmp = builder.CreateICmpSLT(cur_idx, arr_len, "range.cmp");
  builder.CreateCondBr(cmp, bbs.body_bb, bbs.exit_bb);

  func->insert(func->end(), bbs.body_bb);
  builder.SetInsertPoint(bbs.body_bb);
  tick_reduction(*this);

  auto *at_fn = module->getFunction("saga_array_at");
  auto *body_idx = builder.CreateLoad(i64_type, idx_alloca, "idx");
  auto *elem_ptr = builder.CreateCall(at_fn, {iterable, body_idx}, "at");

  if (key_alloca)
    builder.CreateStore(body_idx, key_alloca);
  if (val_alloca) {
    if (struct_elem) {
      auto sz = module->getDataLayout().getTypeAllocSize(elem_ll);
      auto al = module->getDataLayout().getABITypeAlign(elem_ll);
      builder.CreateMemCpy(val_alloca, al, elem_ptr, al, sz);
    } else {
      auto *elem_val = builder.CreateLoad(elem_ll, elem_ptr, "elem");
      builder.CreateStore(elem_val, val_alloca);
    }
  }

  auto &body_block = std::get<BlockNode>(node.body->data);
  emit_block(body_block);
  if (!builder.GetInsertBlock()->getTerminator())
    builder.CreateBr(bbs.update_bb);

  func->insert(func->end(), bbs.update_bb);
  builder.SetInsertPoint(bbs.update_bb);
  auto *upd_idx = builder.CreateLoad(i64_type, idx_alloca, "idx");
  auto *next_idx = builder.CreateAdd(
      upd_idx, llvm::ConstantInt::get(i64_type, 1), "idx.next");
  builder.CreateStore(next_idx, idx_alloca);
  builder.CreateBr(bbs.cond_bb);
}

void CodeGen::emit_for_range_map(const ForExprNode &node,
                                 const ForRangeClauseNode &range,
                                 llvm::Value *iterable,
                                 const TypePtr &iter_sem,
                                 const ForLoopBlocks &bbs) {
  auto *func = builder.GetInsertBlock()->getParent();
  auto &map_info = std::get<MapTypeInfo>(iter_sem->detail);

  auto *size_fn = module->getFunction("saga_map_size");
  auto *map_len = builder.CreateCall(size_fn, {iterable}, "map.len");

  auto *idx_alloca = create_entry_alloca(func, ".map.idx", i64_type);
  builder.CreateStore(llvm::ConstantInt::get(i64_type, 0), idx_alloca);

  auto *key_ll = llvm_type(map_info.key);
  auto *val_ll = llvm_type(map_info.value);
  llvm::AllocaInst *key_alloca = nullptr;
  llvm::AllocaInst *val_alloca = nullptr;

  if (range.vars.size() == 1) {
    val_alloca =
        create_entry_alloca(func, std::string(range.vars[0].name), val_ll);
    locals[std::string(range.vars[0].name)] = val_alloca;
  } else if (range.vars.size() == 2) {
    key_alloca =
        create_entry_alloca(func, std::string(range.vars[0].name), key_ll);
    locals[std::string(range.vars[0].name)] = key_alloca;
    val_alloca =
        create_entry_alloca(func, std::string(range.vars[1].name), val_ll);
    locals[std::string(range.vars[1].name)] = val_alloca;
  }

  builder.CreateBr(bbs.cond_bb);
  builder.SetInsertPoint(bbs.cond_bb);
  auto *cur_idx = builder.CreateLoad(i64_type, idx_alloca, "map.idx");
  auto *cmp = builder.CreateICmpSLT(cur_idx, map_len, "map.cmp");
  builder.CreateCondBr(cmp, bbs.body_bb, bbs.exit_bb);

  func->insert(func->end(), bbs.body_bb);
  builder.SetInsertPoint(bbs.body_bb);
  tick_reduction(*this);

  auto *body_idx = builder.CreateLoad(i64_type, idx_alloca, "map.idx");

  auto load_or_memcpy = [&](llvm::AllocaInst *dst, llvm::Type *ll,
                            const char *fn_name, const char *vname) {
    auto *fn = module->getFunction(fn_name);
    auto *ptr = builder.CreateCall(fn, {iterable, body_idx},
                                   std::string(vname) + ".ptr");
    if (ll->isStructTy()) {
      auto sz = module->getDataLayout().getTypeAllocSize(ll);
      auto al = module->getDataLayout().getABITypeAlign(ll);
      builder.CreateMemCpy(dst, al, ptr, al, sz);
    } else {
      auto *v = builder.CreateLoad(ll, ptr, vname);
      builder.CreateStore(v, dst);
    }
  };

  if (key_alloca)
    load_or_memcpy(key_alloca, key_ll, "saga_map_key_at", "map.key");
  if (val_alloca)
    load_or_memcpy(val_alloca, val_ll, "saga_map_value_at", "map.val");

  auto &body_block = std::get<BlockNode>(node.body->data);
  emit_block(body_block);
  if (!builder.GetInsertBlock()->getTerminator())
    builder.CreateBr(bbs.update_bb);

  func->insert(func->end(), bbs.update_bb);
  builder.SetInsertPoint(bbs.update_bb);
  auto *upd_idx = builder.CreateLoad(i64_type, idx_alloca, "map.idx");
  auto *next_idx = builder.CreateAdd(
      upd_idx, llvm::ConstantInt::get(i64_type, 1), "map.idx.next");
  builder.CreateStore(next_idx, idx_alloca);
  builder.CreateBr(bbs.cond_bb);
}

// `for msg : task` reads from a companion channel pointer the spawn site
// stored in a local variable named `<task>.channel`. Without that
// companion, no message stream exists, so we fall through to exit.
void CodeGen::emit_for_range_task(const ForExprNode &node,
                                  const ForRangeClauseNode &range,
                                  const StructTypeInfo &st_info,
                                  const ForLoopBlocks &bbs) {
  auto *func = builder.GetInsertBlock()->getParent();

  std::string task_name;
  if (auto *iter_id = std::get_if<IdentifierNode>(&range.iterable->data))
    task_name = std::string(iter_id->name);

  llvm::Value *ch_ptr = nullptr;
  if (!task_name.empty()) {
    auto ch_it = locals.find(task_name + ".channel");
    if (ch_it != locals.end())
      ch_ptr = builder.CreateLoad(llvm::PointerType::getUnqual(context),
                                  ch_it->second, "ch.ptr");
  }
  if (!ch_ptr) {
    builder.CreateBr(bbs.exit_bb);
    return;
  }

  llvm::Type *msg_ll = i64_type;
  TypePtr elem_sem;
  if (!st_info.type_args.empty())
    elem_sem = st_info.type_args[0];
  if (elem_sem) {
    auto *ll = llvm_type(elem_sem);
    if (ll && !ll->isVoidTy())
      msg_ll = ll;
  }

  llvm::AllocaInst *msg_alloca = nullptr;
  if (!range.vars.empty()) {
    std::string vname(range.vars[0].name);
    msg_alloca = create_entry_alloca(func, vname, msg_ll);
    locals[vname] = msg_alloca;
  } else {
    msg_alloca = create_entry_alloca(func, ".msg.buf", msg_ll);
  }

  builder.CreateBr(bbs.cond_bb);
  builder.SetInsertPoint(bbs.cond_bb);
  auto *recv_fn = module->getFunction("saga_channel_recv");
  auto *rc =
      builder.CreateCall(recv_fn, {ch_ptr, msg_alloca}, "recv.rc");
  auto *eof = builder.CreateICmpEQ(
      rc,
      llvm::ConstantInt::get(llvm::Type::getInt32Ty(context), -1),
      "recv.eof");
  builder.CreateCondBr(eof, bbs.exit_bb, bbs.body_bb);

  func->insert(func->end(), bbs.body_bb);
  builder.SetInsertPoint(bbs.body_bb);
  tick_reduction(*this);
  auto &body_block = std::get<BlockNode>(node.body->data);
  emit_block(body_block);
  if (!builder.GetInsertBlock()->getTerminator())
    builder.CreateBr(bbs.update_bb);

  func->insert(func->end(), bbs.update_bb);
  builder.SetInsertPoint(bbs.update_bb);
  builder.CreateBr(bbs.cond_bb);
}

// `for v : iter` calls iter.Next() each step, where Next() returns
// T | Error. The loop ends when the result tag matches the Error variant.
void CodeGen::emit_for_range_iterable_struct(const ForExprNode &node,
                                             const ForRangeClauseNode &range,
                                             llvm::Value *iterable,
                                             const TypePtr &iter_sem,
                                             const ForLoopBlocks &bbs) {
  auto *func = builder.GetInsertBlock()->getParent();
  auto &st_info = std::get<StructTypeInfo>(iter_sem->detail);

  auto elem_sem = iterable_next_elem_type_of(*range.iterable);
  if (!elem_sem) {
    builder.CreateBr(bbs.exit_bb);
    return;
  }
  auto *elem_ll = llvm_type(elem_sem);
  auto *ptr_type = llvm::PointerType::getUnqual(context);

  std::string next_link_name;
  auto ml_it = struct_method_links.find(st_info.name);
  if (ml_it != struct_method_links.end()) {
    for (auto &[lname, mname] : ml_it->second)
      if (mname == "Next") { next_link_name = lname; break; }
  }
  if (next_link_name.empty())
    next_link_name = mangle(st_info.name + "__Next");

  TypePtr next_ret_sem;
  for (auto &m : st_info.methods) {
    if (m.name == "Next" && m.signature &&
        m.signature->kind == TypeKind::Func) {
      auto &fi = std::get<FuncTypeInfo>(m.signature->detail);
      if (!fi.returns.empty())
        next_ret_sem = fi.returns[0];
      break;
    }
  }
  if (!next_ret_sem)
    next_ret_sem =
        make_union_type({elem_sem, analyzer.builtins.error_iface});

  auto *union_st = get_union_llvm_type(next_ret_sem);

  int error_tag = 1;
  if (next_ret_sem->kind == TypeKind::Union) {
    auto &ui = std::get<UnionTypeInfo>(next_ret_sem->detail);
    for (size_t i = 0; i < ui.alternatives.size(); ++i)
      if (ui.alternatives[i]->kind == TypeKind::Interface)
        error_tag = static_cast<int>(i);
  }

  auto *next_fn = module->getFunction(next_link_name);
  if (!next_fn && union_st) {
    auto *ret_ll = static_cast<llvm::Type *>(union_st);
    auto *fn_type = llvm::FunctionType::get(ret_ll, {ptr_type}, false);
    next_fn = llvm::Function::Create(fn_type, llvm::Function::ExternalLinkage,
                                     next_link_name, module.get());
  }

  llvm::Value *self_ptr = nullptr;
  if (auto *id = std::get_if<IdentifierNode>(&range.iterable->data)) {
    auto local_it = locals.find(std::string(id->name));
    if (local_it != locals.end()) {
      auto *alloca = local_it->second;
      auto st_it2 = struct_types.find(st_info.name);
      if (st_it2 != struct_types.end() &&
          alloca->getAllocatedType() == st_it2->second)
        self_ptr = alloca;
    }
  }
  if (!self_ptr) {
    auto st_it2 = struct_types.find(st_info.name);
    if (st_it2 != struct_types.end() &&
        iterable->getType() == st_it2->second) {
      auto *tmp = create_entry_alloca(func, "iter.self", st_it2->second);
      builder.CreateStore(iterable, tmp);
      self_ptr = tmp;
    } else {
      self_ptr = iterable;
    }
  }

  llvm::AllocaInst *val_alloca = nullptr;
  if (!range.vars.empty()) {
    std::string vname(range.vars[0].name);
    val_alloca = create_entry_alloca(func, vname, elem_ll);
    locals[vname] = val_alloca;
  }

  llvm::AllocaInst *result_alloca = nullptr;
  if (union_st)
    result_alloca = create_entry_alloca(func, "next.result", union_st);

  if (!next_fn || !result_alloca || !self_ptr) {
    builder.CreateBr(bbs.exit_bb);
    return;
  }

  builder.CreateBr(bbs.cond_bb);
  builder.SetInsertPoint(bbs.cond_bb);
  auto *next_val = builder.CreateCall(next_fn, {self_ptr}, "next.val");
  builder.CreateStore(next_val, result_alloca);

  auto *tag_gep = builder.CreateStructGEP(union_st, result_alloca, 0,
                                          "next.tag.ptr");
  auto *tag = builder.CreateLoad(llvm::Type::getInt8Ty(context), tag_gep,
                                 "next.tag");
  auto *is_err = builder.CreateICmpEQ(
      tag,
      llvm::ConstantInt::get(llvm::Type::getInt8Ty(context), error_tag),
      "next.is_err");
  builder.CreateCondBr(is_err, bbs.exit_bb, bbs.body_bb);

  func->insert(func->end(), bbs.body_bb);
  builder.SetInsertPoint(bbs.body_bb);
  tick_reduction(*this);

  if (val_alloca) {
    auto *val = emit_union_extract(result_alloca, elem_sem, next_ret_sem);
    if (val)
      builder.CreateStore(val, val_alloca);
  }

  auto &body_block = std::get<BlockNode>(node.body->data);
  emit_block(body_block);
  if (!builder.GetInsertBlock()->getTerminator())
    builder.CreateBr(bbs.update_bb);

  func->insert(func->end(), bbs.update_bb);
  builder.SetInsertPoint(bbs.update_bb);
  builder.CreateBr(bbs.cond_bb);
}

} // namespace saga

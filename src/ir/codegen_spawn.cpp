// Copyright 2026 Rob Thornton
// SPDX-License-Identifier: MIT

// Spawn expression emission: lowers `spawn { ... }` and `spawn |T| { ... }`
// into an outlined `void(saga_runtime_actor*)` function plus a runtime
// call to `saga_executor_spawn`. Captured variables are packed into a
// closure struct memcpy'd into the actor's arena. Generic spawns also
// allocate a channel and stash it as a companion local for the next
// DeclAssign to bind alongside the task handle.

#include "ir/codegen.hpp"

#include <llvm/IR/Verifier.h>

namespace saga {

llvm::Value *CodeGen::emit_spawn_expr(const SpawnExprNode &node,
                                       const Node &parent) {
  has_spawn = true;

  auto *ptr_type = llvm::PointerType::getUnqual(context);
  auto *enclosing_func = builder.GetInsertBlock()->getParent();

  // Generate a unique name for this spawn's outlined function.
  std::string spawn_name = "saga.spawn." + std::to_string(next_spawn_id++);

  // ── Look up captured variables ─────────────────────────────────────
  std::vector<Analyzer::SpawnCaptureInfo> captures;
  if (auto *caps = spawn_captures_of(parent))
    captures = *caps;

  // ── Build the closure struct type ──────────────────────────────────
  // Holds one field per captured variable.
  std::vector<llvm::Type *> closure_field_types;
  std::vector<std::string> closure_field_names;
  for (auto &cap : captures) {
    auto *ll = llvm_type(cap.type);
    closure_field_types.push_back(ll);
    closure_field_names.push_back(cap.name);
  }

  llvm::StructType *closure_st = nullptr;
  uint64_t closure_size = 0;
  if (!closure_field_types.empty()) {
    closure_st = llvm::StructType::create(context, closure_field_types,
                                           spawn_name + ".closure");
    closure_size = module->getDataLayout().getTypeAllocSize(closure_st);
  }

  // ── Build the outlined function: void(saga_runtime_actor*) ───────────────────
  auto *outlined_fn_type = llvm::FunctionType::get(
      void_ll_type, {ptr_type}, false);
  auto *outlined_fn = llvm::Function::Create(
      outlined_fn_type, llvm::Function::InternalLinkage, spawn_name,
      module.get());
  outlined_fn->getArg(0)->setName("actor");

  // ── Emit the outlined function body ────────────────────────────────
  auto saved_block = builder.GetInsertBlock();
  auto saved_point = builder.GetInsertPoint();
  auto saved_locals = locals;
  auto saved_managed = managed_locals;
  auto saved_is_main = current_func_is_main;
  auto saved_actor = current_actor;

  auto *entry = llvm::BasicBlock::Create(context, "entry", outlined_fn);
  builder.SetInsertPoint(entry);

  locals.clear();
  managed_locals.clear();
  current_func_is_main = false;
  current_actor = outlined_fn->getArg(0);

  // Unpack closure data from actor->closure_data via the runtime accessor.
  // The runtime memcpy'd the closure struct into the actor's arena; we
  // recover the pointer through saga_runtime_actor_get_closure(actor).
  if (closure_st && !captures.empty()) {
    auto *accessor = module->getFunction("saga_runtime_actor_get_closure");
    if (!accessor) {
      accessor = llvm::Function::Create(
          llvm::FunctionType::get(ptr_type, {ptr_type}, false),
          llvm::Function::ExternalLinkage, "saga_runtime_actor_get_closure",
          module.get());
    }

    auto *closure_ptr = builder.CreateCall(accessor, {current_actor},
                                            "closure.ptr");

    for (size_t i = 0; i < captures.size(); ++i) {
      auto *field_gep = builder.CreateStructGEP(
          closure_st, closure_ptr, i, captures[i].name + ".cap");
      auto *val = builder.CreateLoad(closure_field_types[i], field_gep,
                                      captures[i].name);
      auto *alloca = create_entry_alloca(outlined_fn, captures[i].name,
                                          closure_field_types[i]);
      builder.CreateStore(val, alloca);
      locals[captures[i].name] = alloca;

      // Retain shared refcounted captures so they aren't freed while
      // the spawned actor is still using them.
      if (captures[i].kind == Analyzer::SpawnCaptureKind::Share) {
        TypePtr cap_type = captures[i].type;
        emit_retain(val, cap_type);
      }
    }
  }

  // If the spawn has a pipe variable (|task|), bind it to the actor ptr.
  if (node.pipe) {
    std::string pipe_name(node.pipe->name);
    auto *alloca = create_entry_alloca(outlined_fn, pipe_name, ptr_type);
    builder.CreateStore(current_actor, alloca);
    locals[pipe_name] = alloca;
  }

  // Emit the spawn body.
  if (auto *block = std::get_if<BlockNode>(&node.body->data)) {
    emit_block(*block);
  } else {
    // Identifier body (function reference) — call it as a trampoline.
    auto *body_val = emit_expr(*node.body);
    // If it returned a value, we could use context_exit, but for
    // simplicity we just ignore it; the function should use task.Exit().
    (void)body_val;
  }

  // Add terminator if needed.
  if (!builder.GetInsertBlock()->getTerminator())
    builder.CreateRetVoid();

  llvm::verifyFunction(*outlined_fn);

  // ── Restore enclosing function state ───────────────────────────────
  builder.SetInsertPoint(saved_block, saved_point);
  locals = saved_locals;
  managed_locals = saved_managed;
  current_func_is_main = saved_is_main;
  current_actor = saved_actor;

  // ── Pack closure data at the spawn site ────────────────────────────
  llvm::Value *closure_ptr_val =
      llvm::ConstantPointerNull::get(ptr_type);

  if (closure_st && !captures.empty()) {
    auto *closure_alloca = create_entry_alloca(
        enclosing_func, spawn_name + ".closure", closure_st);

    for (size_t i = 0; i < captures.size(); ++i) {
      auto *field_gep = builder.CreateStructGEP(
          closure_st, closure_alloca, i, captures[i].name + ".pack");

      auto local_it = locals.find(captures[i].name);
      if (local_it != locals.end()) {
        auto *alloca = local_it->second;
        auto *val = builder.CreateLoad(closure_field_types[i], alloca,
                                        captures[i].name + ".cap.val");
        builder.CreateStore(val, field_gep);

        // Retain shared refcounted captures at the spawn site.
        if (captures[i].kind == Analyzer::SpawnCaptureKind::Share)
          emit_retain(val, captures[i].type);
      } else {
        builder.CreateStore(
            llvm::Constant::getNullValue(closure_field_types[i]),
            field_gep);
      }
    }
    closure_ptr_val = closure_alloca;
  }

  // ── Create channel if generic type present ─────────────────────────
  llvm::Value *channel_ptr = nullptr;
  int64_t channel_elem_size = 0;
  if (node.generic && !node.generic->type_params.empty()) {
    // Read the channel element type the analyzer resolved while the
    // spawn's enclosing scope was still live.  Re-resolving here would
    // fail for user struct names because their scope has been popped.
    TypePtr chan_elem_sem = spawn_channel_elem_type_of(parent);
    if (!chan_elem_sem)
      chan_elem_sem = analyzer.resolve_type(*node.generic->type_params[0]);
    auto *chan_elem_ll = llvm_type(chan_elem_sem);
    if (!chan_elem_ll || chan_elem_ll->isVoidTy())
      chan_elem_ll = i64_type; // defensive fallback; shouldn't happen
    channel_elem_size = static_cast<int64_t>(
        module->getDataLayout().getTypeAllocSize(chan_elem_ll));

    auto *ch = builder.CreateCall(
        module->getFunction("saga_channel_new"),
        {llvm::ConstantInt::get(i64_type, channel_elem_size),
         llvm::ConstantInt::get(i64_type, 0)}, // 0 = default capacity
        "channel");
    channel_ptr = ch;
  }

  // ── Spawn the actor ────────────────────────────────────────────────
  auto *actor = builder.CreateCall(
      module->getFunction("saga_executor_spawn"),
      {outlined_fn,
       closure_ptr_val,
       llvm::ConstantInt::get(i64_type, static_cast<int64_t>(closure_size)),
       llvm::ConstantInt::get(i64_type, 0)}, // 0 = default arena max
      "actor");

  // Attach channel to actor if present.
  if (channel_ptr) {
    auto *attach_fn = module->getFunction("saga_runtime_actor_set_channel");
    if (!attach_fn) {
      attach_fn = llvm::Function::Create(
          llvm::FunctionType::get(void_ll_type, {ptr_type, ptr_type}, false),
          llvm::Function::ExternalLinkage, "saga_runtime_actor_set_channel",
          module.get());
    }
    builder.CreateCall(attach_fn, {actor, channel_ptr});
  }

  // ── Stash the channel for the next DeclAssign ──────────────────────
  // The actor pointer itself is the Task handle the caller will bind.
  // For channel-based spawns we also stash the channel under a per-spawn
  // local key; the DeclAssign that receives this actor value picks up
  // pending_channel_alloca_ and creates a "<taskname>.channel" alias so
  // for-range can find it.
  if (channel_ptr) {
    std::string ch_local = spawn_name + ".channel";
    auto *ch_alloca = create_entry_alloca(enclosing_func, ch_local, ptr_type);
    builder.CreateStore(channel_ptr, ch_alloca);
    locals[ch_local] = ch_alloca;
    pending_channel_alloca_ = ch_alloca;
  }

  return actor;
}

} // namespace saga

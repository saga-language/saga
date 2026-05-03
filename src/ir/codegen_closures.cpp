// Copyright 2026 Rob Thornton
// SPDX-License-Identifier: MIT

// Closure expression emission: lowers `fn(args) { body }` literal into a
// trampoline function plus a fat pointer { fn_ptr, env_ptr } where env
// holds the captured upvalues. The fat pointer flows wherever a callable
// value is expected.

#include "ir/codegen.hpp"

#include <llvm/IR/Verifier.h>

namespace saga {

llvm::Value *CodeGen::emit_func_expr(const FuncExprNode &node,
                                      const Node &parent) {
  auto *ptr_type = llvm::PointerType::getUnqual(context);
  auto *enclosing_func = builder.GetInsertBlock()->getParent();

  // Generate a unique name for this closure.
  std::string closure_name = "saga.closure." + std::to_string(next_closure_id++);

  // ── Look up captured variables ─────────────────────────────────────
  std::vector<Analyzer::CaptureInfo> captures;
  if (auto *caps = node_captures_of(parent))
    captures = *caps;

  // ── Build the environment struct type ──────────────────────────────
  // The env struct holds one field per captured variable.
  std::vector<llvm::Type *> env_field_types;
  std::vector<std::string> env_field_names;
  for (auto &cap : captures) {
    auto *ll = llvm_type(cap.type);
    env_field_types.push_back(ll);
    env_field_names.push_back(cap.name);
  }

  llvm::StructType *env_type = nullptr;
  if (!env_field_types.empty()) {
    env_type = llvm::StructType::create(context, env_field_types,
                                         closure_name + ".env");
  }

  // ── Build the trampoline function ──────────────────────────────────
  // Signature: ret_type trampoline(ptr env, param1, param2, ...)
  std::vector<llvm::Type *> tramp_param_types;
  tramp_param_types.push_back(ptr_type); // env pointer (always first)

  for (auto &param : node.signature.params) {
    auto *ll = resolve_type_node(*param.type);
    for (size_t i = 0; i < param.names.identifiers.size(); ++i)
      tramp_param_types.push_back(ll);
  }

  llvm::Type *ret_type = void_ll_type;
  if (!node.signature.returns.empty())
    ret_type = resolve_type_node(*node.signature.returns[0]);

  auto *tramp_fn_type = llvm::FunctionType::get(ret_type, tramp_param_types,
                                                  false);
  auto *tramp_fn = llvm::Function::Create(
      tramp_fn_type, llvm::Function::InternalLinkage, closure_name,
      module.get());

  // Name the arguments.
  tramp_fn->getArg(0)->setName("env");
  size_t arg_idx = 1;
  for (auto &param : node.signature.params)
    for (auto &ident : param.names.identifiers)
      if (arg_idx < tramp_fn->arg_size())
        tramp_fn->getArg(arg_idx++)->setName(std::string(ident.name));

  // ── Emit the trampoline body ───────────────────────────────────────
  // Save current insertion state.
  auto saved_block = builder.GetInsertBlock();
  auto saved_point = builder.GetInsertPoint();
  auto saved_locals = locals;
  auto saved_managed = managed_locals;
  auto saved_is_main = current_func_is_main;

  auto *entry = llvm::BasicBlock::Create(context, "entry", tramp_fn);
  builder.SetInsertPoint(entry);

  locals.clear();
  managed_locals.clear();
  current_func_is_main = false;

  // Unpack environment struct into local variables.
  if (env_type) {
    auto *env_ptr = tramp_fn->getArg(0);
    for (size_t i = 0; i < captures.size(); ++i) {
      auto *field_gep = builder.CreateStructGEP(env_type, env_ptr, i,
                                                  captures[i].name + ".cap");
      auto *field_type = env_field_types[i];
      auto *val = builder.CreateLoad(field_type, field_gep,
                                      captures[i].name);
      auto *alloca = create_entry_alloca(tramp_fn, captures[i].name,
                                          field_type);
      builder.CreateStore(val, alloca);
      locals[captures[i].name] = alloca;
    }
  }

  // Create allocas for parameters.
  arg_idx = 1;
  for (auto &param : node.signature.params) {
    auto *ll = resolve_type_node(*param.type);
    for (auto &ident : param.names.identifiers) {
      std::string pname(ident.name);
      auto *alloca = create_entry_alloca(tramp_fn, pname, ll);
      builder.CreateStore(tramp_fn->getArg(arg_idx++), alloca);
      locals[pname] = alloca;
    }
  }

  // Emit the closure body.
  auto &block = std::get<BlockNode>(node.body->data);
  auto *tail_val = emit_block(block);

  // Add terminator if needed.
  if (!builder.GetInsertBlock()->getTerminator()) {
    emit_release_locals();
    if (ret_type->isVoidTy()) {
      builder.CreateRetVoid();
    } else if (tail_val && tail_val->getType() == ret_type) {
      builder.CreateRet(tail_val);
    } else {
      builder.CreateRet(llvm::Constant::getNullValue(ret_type));
    }
  }

  llvm::verifyFunction(*tramp_fn);

  // ── Restore the enclosing function's state ─────────────────────────
  builder.SetInsertPoint(saved_block, saved_point);
  locals = saved_locals;
  managed_locals = saved_managed;
  current_func_is_main = saved_is_main;

  // ── Allocate and populate the environment struct ───────────────────
  llvm::Value *env_ptr = llvm::ConstantPointerNull::get(ptr_type);
  if (env_type) {
    auto *env_alloca = create_entry_alloca(enclosing_func,
                                            closure_name + ".env",
                                            env_type);
    // Store each captured variable's current value into the env.
    for (size_t i = 0; i < captures.size(); ++i) {
      auto *field_gep = builder.CreateStructGEP(env_type, env_alloca, i,
                                                  captures[i].name + ".env.f");
      // Load the current value of the captured variable.
      auto local_it = locals.find(captures[i].name);
      if (local_it != locals.end()) {
        auto *alloca = local_it->second;
        auto *val = builder.CreateLoad(env_field_types[i], alloca,
                                        captures[i].name + ".cap.val");
        builder.CreateStore(val, field_gep);
      } else {
        // Zero-initialize if not found (shouldn't happen after analysis).
        builder.CreateStore(
            llvm::Constant::getNullValue(env_field_types[i]), field_gep);
      }
    }
    env_ptr = env_alloca;
  }

  // ── Build the closure fat pointer { fn_ptr, env_ptr } ─────────────
  auto *closure_alloca = create_entry_alloca(enclosing_func,
                                              closure_name + ".val",
                                              closure_fat_ptr_type);
  auto *fn_gep = builder.CreateStructGEP(closure_fat_ptr_type,
                                          closure_alloca, 0, "closure.fn");
  builder.CreateStore(tramp_fn, fn_gep);
  auto *env_gep = builder.CreateStructGEP(closure_fat_ptr_type,
                                            closure_alloca, 1, "closure.env");
  builder.CreateStore(env_ptr, env_gep);

  return closure_alloca;
}

} // namespace saga

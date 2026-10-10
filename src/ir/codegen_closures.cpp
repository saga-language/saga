// Copyright 2026 Rob Thornton
// SPDX-License-Identifier: MIT

// A function expression: its body becomes a trampoline taking the closure's
// environment as a leading pointer, and its value a box holding that
// environment (codegen_function_values.cpp).

#include "ir/codegen.hpp"
#include "util/internal_error.hpp"

#include <llvm/IR/Verifier.h>

namespace saga {

llvm::Value *CodeGen::emit_func_expr(const FuncExprNode &node,
                                      const Node &parent) {
  std::string closure_name =
      "saga.closure." + std::to_string(next_closure_id++);
  std::vector<Analyzer::CaptureInfo> captures;
  if (auto *caps = node_captures_of(parent))
    captures = *caps;
  auto env_sem = closure_env_type(closure_name, captures);

  auto fn_sem = unwrap_alias(semantic_type(parent));
  if (!fn_sem || fn_sem->kind != TypeKind::Func)
    internal_error("a function expression has no function type");
  auto *tramp_fn = emit_closure_trampoline(
      closure_name, node, std::get<FuncTypeInfo>(fn_sem->detail), env_sem,
      captures);
  return emit_closure_box(closure_name, tramp_fn, env_sem, captures,
                          analyzer.closure_writes_captures(parent));
}

TypePtr CodeGen::closure_env_type(
    const std::string &closure_name,
    const std::vector<Analyzer::CaptureInfo> &captures) {
  if (captures.empty())
    return nullptr;
  std::vector<FieldInfo> fields;
  for (auto &cap : captures)
    fields.push_back({cap.name, cap.type, false});
  return make_struct_type(closure_name + ".env", std::move(fields));
}

llvm::Function *CodeGen::emit_closure_trampoline(
    const std::string &closure_name, const FuncExprNode &node,
    const FuncTypeInfo &fi, const TypePtr &env_sem,
    const std::vector<Analyzer::CaptureInfo> &captures) {
  auto tramp_sig = lower_signature(fi, llvm::PointerType::getUnqual(context));
  auto *tramp_fn = declare_function(closure_name, tramp_sig,
                                    llvm::Function::InternalLinkage);
  name_params(tramp_fn, tramp_sig, node.signature, "env");

  // The body is part of the enclosing one, so it reads the same side tables.
  FuncEmissionScope guard(*this, current_instantiation_);
  builder.SetInsertPoint(llvm::BasicBlock::Create(context, "entry", tramp_fn));
  return_sems_[tramp_fn] = declared_return_sem(node.signature.return_type);

  unsigned env_idx = first_param_index(tramp_fn, false);
  bind_captures(tramp_fn->getArg(env_idx), env_sem, captures);
  bind_params(tramp_fn, env_idx + 1, node.signature, fi);

  auto &block = std::get<BlockNode>(node.body->data);
  auto *tail_val = emit_block(block);
  if (!builder.GetInsertBlock()->getTerminator())
    emit_fallthrough_return(block, tail_val);
  verify_function(*tramp_fn);
  return tramp_fn;
}

// A captured name is a view of the closure's own field for the call: the
// field keeps its reference, a write releases and replaces it as it would a
// local's, and every exit writes the views back (`write_back_captures`), so
// the closure's state carries from one call to the next.
void CodeGen::bind_captures(
    llvm::Value *env, const TypePtr &env_sem,
    const std::vector<Analyzer::CaptureInfo> &captures) {
  if (!env_sem)
    return;
  auto *fn = builder.GetInsertBlock()->getParent();
  auto *env_st = llvm::cast<llvm::StructType>(llvm_type(env_sem));
  for (size_t i = 0; i < captures.size(); ++i) {
    auto *field = builder.CreateStructGEP(env_st, env, i, captures[i].name);
    auto *field_ll = env_st->getElementType(i);
    auto *view = create_entry_alloca(fn, captures[i].name, field_ll);
    builder.CreateStore(builder.CreateLoad(field_ll, field), view);
    locals[captures[i].name] = view;
    capture_views_[fn].push_back({view, field});
  }
}

void CodeGen::write_back_captures(llvm::Function *fn) {
  auto it = capture_views_.find(fn);
  if (it == capture_views_.end())
    return;
  for (auto &[view, field] : it->second)
    builder.CreateStore(builder.CreateLoad(view->getAllocatedType(), view),
                        field);
}

} // namespace saga

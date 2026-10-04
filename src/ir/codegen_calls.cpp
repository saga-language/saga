// Copyright 2026 Rob Thornton
// SPDX-License-Identifier: MIT

#include "ir/codegen.hpp"

#include <algorithm>
#include <llvm/IR/Constants.h>

namespace saga {

// A C prototype takes what C declared, and the runtime spells its `bool` and
// `byte` parameters `int64_t`. The Saga-side `extern fn` names the Saga type,
// so the two disagree on width and the value has to reach the callee at the
// width it was declared with. Saga's narrow integers are unsigned, so widening
// is a zext.
llvm::Value *CodeGen::fit_extern_int(llvm::Value *val, llvm::Type *expected) {
  if (!val || !val->getType()->isIntegerTy() || !expected->isIntegerTy())
    return val;

  unsigned have = val->getType()->getIntegerBitWidth();
  unsigned want = expected->getIntegerBitWidth();
  if (have == want)
    return val;
  return have < want ? builder.CreateZExt(val, expected, "extern.ext")
                     : builder.CreateTrunc(val, expected, "extern.trunc");
}

// ===========================================================================
// Call expressions
// ===========================================================================

llvm::Value *CodeGen::emit_call_expr(const CallExprNode &node,
                                     const Node &parent) {
  if (std::holds_alternative<SelectorNode>(node.callee->data))
    return emit_method_or_module_call(node, parent);

  auto *ident = std::get_if<IdentifierNode>(&node.callee->data);
  if (!ident)
    return nullptr;
  std::string name(ident->name);

  if (auto result = emit_intrinsic_call(name, node))
    return *result;
  if (auto *spec = generic_function_specialisation(node, parent))
    return emit_specialisation_call(spec, node);
  if (auto *callee = module->getFunction(direct_link_name(name)))
    return emit_direct_call(callee, node, is_extern_function(name));
  return emit_function_value_call(node, name);
}

bool CodeGen::is_extern_function(const std::string &name) {
  if (!analyzer.package_scope_)
    return false;
  auto sym = analyzer.package_scope_->lookup(name);
  return sym && sym->is_extern;
}

std::string CodeGen::direct_link_name(const std::string &name) {
  if (name == "intrinsic_print")
    return "saga_intrinsic_print";
  return is_extern_function(name) ? name : mangle(name);
}

const Analyzer::BodyInstantiation *CodeGen::find_instantiation(
    const FuncDeclNode *decl,
    const std::unordered_map<uint32_t, TypePtr> &bindings) {
  auto inst_it = analyzer.instantiations_.find(decl);
  if (inst_it == analyzer.instantiations_.end())
    return nullptr;
  for (auto &inst : inst_it->second) {
    if (inst.bindings.size() != bindings.size())
      continue;
    bool match = true;
    for (auto &[id, t] : bindings) {
      auto j = inst.bindings.find(id);
      if (j == inst.bindings.end() || !types_equal(t, j->second)) {
        match = false;
        break;
      }
    }
    if (match)
      return &inst;
  }
  return nullptr;
}

llvm::Function *
CodeGen::generic_function_specialisation(const CallExprNode &node,
                                         const Node &parent) {
  auto callee_sem = unwrap_alias(semantic_type(*node.callee));
  if (!callee_sem || callee_sem->kind != TypeKind::Func)
    return nullptr;
  auto fd_it = analyzer.func_decl_by_type_.find(callee_sem.get());
  if (fd_it == analyzer.func_decl_by_type_.end())
    return nullptr;
  const FuncDeclNode *fn_decl = fd_it->second;
  if (!fn_decl->generic || fn_decl->receiver || fn_decl->is_extern)
    return nullptr;
  auto *bindings = node_type_args_of(parent);
  if (!bindings)
    return nullptr;
  return emit_specialisation(*fn_decl, callee_sem, *bindings,
                             find_instantiation(fn_decl, *bindings));
}

llvm::Value *CodeGen::emit_specialisation_call(llvm::Function *spec,
                                               const CallExprNode &node) {
  std::vector<llvm::Value *> args;
  for (auto &a : node.args)
    if (auto *v = emit_expr(*a))
      args.push_back(v);
  return emit_call(spec, nullptr, args);
}

// A function-typed local or parameter: a closure value carries (fn, env) and
// the trampoline takes env first; a plain function value is just fn.
llvm::Value *CodeGen::emit_function_value_call(const CallExprNode &node,
                                               const std::string &name) {
  auto local_it = locals.find(name);
  if (local_it == locals.end())
    return nullptr;
  auto *alloca = local_it->second;
  auto callee_sem = unwrap_alias(semantic_type(*node.callee));
  if (!callee_sem || callee_sem->kind != TypeKind::Func)
    return nullptr;

  auto *ptr_type = llvm::PointerType::getUnqual(context);
  bool is_closure = alloca->getAllocatedType() == closure_fat_ptr_type;
  llvm::Value *fn_ptr = nullptr;
  llvm::Value *env_ptr = nullptr;
  if (is_closure) {
    auto *fn_gep = builder.CreateStructGEP(
        closure_fat_ptr_type, alloca, 0, "closure.fn.gep");
    fn_ptr = builder.CreateLoad(ptr_type, fn_gep, "closure.fn");
    auto *env_gep = builder.CreateStructGEP(
        closure_fat_ptr_type, alloca, 1, "closure.env.gep");
    env_ptr = builder.CreateLoad(ptr_type, env_gep, "closure.env");
  } else {
    fn_ptr = builder.CreateLoad(ptr_type, alloca, "fn.load");
  }

  std::vector<llvm::Value *> args;
  for (auto &arg_node : node.args)
    if (auto *val = emit_expr(*arg_node))
      args.push_back(val);
  auto sig = lower_signature(std::get<FuncTypeInfo>(callee_sem->detail),
                             is_closure ? ptr_type : nullptr);
  return emit_call(fn_ptr, sig, env_ptr, args);
}

// Variadic arguments past the fixed ones are packed into a fresh array, unless
// the call passes a single array of the variadic type through as it is.
llvm::Value *CodeGen::pack_variadic_args(const CallExprNode &node,
                                         const FuncTypeInfo &fi) {
  if (!fi.is_variadic || fi.params.empty())
    return nullptr;
  size_t variadic_idx = fi.params.size() - 1;
  auto &last = fi.params.back();
  if (!last || last->kind != TypeKind::Array)
    return nullptr;
  if (node.args.size() == fi.params.size()) {
    auto last_arg_sem = semantic_type(*node.args.back());
    if (last_arg_sem && types_equal(last_arg_sem, last))
      return nullptr;
  }

  auto *parent_fn = builder.GetInsertBlock()->getParent();
  auto &arr = std::get<ArrayTypeInfo>(last->detail);
  auto *elem_ll = llvm_type(arr.element);
  uint64_t elem_size = elem_ll ? size_of(elem_ll) : 8;
  int64_t var_count =
      node.args.size() > variadic_idx
          ? static_cast<int64_t>(node.args.size() - variadic_idx)
          : 0;
  std::vector<llvm::Value *> new_args = {
      llvm::ConstantInt::get(i64_type, elem_size),
      llvm::ConstantInt::get(i64_type, std::max<int64_t>(var_count, 4))};
  auto *arr_val = builder.CreateCall(module->getFunction("saga_array_new"),
                                     new_args, "var.arr");
  auto *push_fn = module->getFunction("saga_array_builder_push");
  for (size_t i = variadic_idx; i < node.args.size(); ++i) {
    auto *val = emit_expr(*node.args[i]);
    if (!val) continue;
    auto *tmp = create_entry_alloca(parent_fn, "var.tmp", val->getType());
    builder.CreateStore(val, tmp);
    std::vector<llvm::Value *> push_args = {arr_val, tmp};
    builder.CreateCall(push_fn, push_args);
  }
  return arr_val;
}

llvm::Value *CodeGen::emit_direct_call(llvm::Function *callee,
                                       const CallExprNode &node,
                                       bool callee_is_extern) {
  auto sig = signature_of(callee);
  auto callee_sem = unwrap_alias(semantic_type(*node.callee));
  const FuncTypeInfo *fi = nullptr;
  if (callee_sem && callee_sem->kind == TypeKind::Func)
    fi = &std::get<FuncTypeInfo>(callee_sem->detail);

  llvm::Value *variadic_packed = fi ? pack_variadic_args(node, *fi) : nullptr;
  size_t variadic_idx = fi && !fi->params.empty() ? fi->params.size() - 1 : 0;

  std::vector<llvm::Value *> args;
  for (size_t i = 0; i < node.args.size(); ++i) {
    if (variadic_packed && i >= variadic_idx) {
      args.push_back(variadic_packed);
      break;
    }
    auto *val = emit_operand(*node.args[i]);
    if (!val)
      continue;
    // Spec docs/language.md:51 — a value that escapes its scope is copied,
    // and the call boundary is an escape. The parameter slot is a binding like
    // any other, so a borrowed argument takes a reference for it: a callee that
    // only reads pays an increment, and one that writes finds the buffer shared
    // and gets the copy. Extern (C) callees are the runtime's own in-place path
    // for Push/Pop/Set, which needs the actual array.
    auto arg_sem = operand_type(*node.args[i]);
    if (arg_sem && arg_sem->kind == TypeKind::Array && !callee_is_extern)
      retain_if_borrowed(val, arg_sem, *node.args[i]);
    // The parameter's shape, not the name it was declared under: an alias is a
    // second name for a union or a struct, and every question below is about
    // the shape.
    auto param =
        fi && i < fi->params.size() ? unwrap_alias(fi->params[i]) : nullptr;
    val = coerce_to(val, arg_sem, param);
    size_t param_idx = (sig.sret ? 1 : 0) + args.size();
    if (callee_is_extern && param_idx < sig.type->getNumParams())
      val = fit_extern_param(val, sig.type->getParamType(param_idx));
    args.push_back(val);
  }
  return emit_call(callee, sig, nullptr, args);
}

// A C callee whose parameter is a pointer where Saga has a scalar — a T the
// runtime takes as void*, like saga_array_builder_push's element — is handed
// the scalar's address.
llvm::Value *CodeGen::fit_extern_param(llvm::Value *val, llvm::Type *expected) {
  if (!expected->isPointerTy() || val->getType()->isPointerTy())
    return fit_extern_int(val, expected);
  auto *tmp = create_entry_alloca(builder.GetInsertBlock()->getParent(),
                                  "extern.tmp", val->getType());
  builder.CreateStore(val, tmp);
  return tmp;
}

} // namespace saga

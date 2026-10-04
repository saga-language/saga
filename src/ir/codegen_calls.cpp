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
    return emit_function_value_call(node);
  std::string name(ident->name);

  if (auto result = emit_intrinsic_call(name, node))
    return *result;
  if (auto [spec, concrete] = generic_function_specialisation(node, parent);
      spec)
    return emit_specialisation_call(spec, concrete, node);
  if (auto *callee = module->getFunction(direct_link_name(name)))
    return emit_direct_call(callee, node, is_extern_function(name));
  return emit_function_value_call(node);
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

// The specialisation a generic call names, with the signature its bindings
// give it: the call lowers its arguments against that, as the body binds them.
std::pair<llvm::Function *, TypePtr>
CodeGen::generic_function_specialisation(const CallExprNode &node,
                                         const Node &parent) {
  auto callee_sem = unwrap_alias(semantic_type(*node.callee));
  if (!callee_sem || callee_sem->kind != TypeKind::Func)
    return {};
  auto fd_it = analyzer.func_decl_by_type_.find(callee_sem.get());
  if (fd_it == analyzer.func_decl_by_type_.end())
    return {};
  const FuncDeclNode *fn_decl = fd_it->second;
  if (!fn_decl->generic || fn_decl->receiver || fn_decl->is_extern)
    return {};
  auto *bindings = node_type_args_of(parent);
  if (!bindings)
    return {};
  auto *spec = emit_specialisation(*fn_decl, callee_sem, *bindings,
                                   find_instantiation(fn_decl, *bindings));
  return {spec, spec ? substitute(callee_sem, *bindings) : nullptr};
}

llvm::Value *CodeGen::emit_specialisation_call(llvm::Function *spec,
                                               const TypePtr &concrete,
                                               const CallExprNode &node) {
  auto &fi = std::get<FuncTypeInfo>(concrete->detail);
  return emit_call(spec, nullptr, emit_arguments(node, &fi, true));
}

// A function-typed local or parameter: a closure value carries (fn, env) and
// the trampoline takes env first; a plain function value is just fn.
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

  auto &arr = std::get<ArrayTypeInfo>(last->detail);
  int64_t var_count =
      node.args.size() > variadic_idx
          ? static_cast<int64_t>(node.args.size() - variadic_idx)
          : 0;
  auto *arr_val = emit_new_array(arr.element, var_count, "var.arr");
  for (size_t i = variadic_idx; i < node.args.size(); ++i)
    emit_push_element(arr_val, arr.element, *node.args[i]);
  return arr_val;
}

llvm::Value *CodeGen::emit_direct_call(llvm::Function *callee,
                                       const CallExprNode &node,
                                       bool callee_is_extern) {
  auto callee_sem = unwrap_alias(semantic_type(*node.callee));
  const FuncTypeInfo *fi = nullptr;
  if (callee_sem && callee_sem->kind == TypeKind::Func)
    fi = &std::get<FuncTypeInfo>(callee_sem->detail);

  auto sig = signature_of(callee);
  auto args = emit_arguments(node, fi, !callee_is_extern);
  if (callee_is_extern) {
    unsigned first = sig.sret ? 1 : 0;
    for (unsigned i = 0; i < args.size() && first + i < sig.type->getNumParams();
         ++i)
      args[i] = fit_extern_param(args[i], sig.type->getParamType(first + i));
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

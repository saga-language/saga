// Copyright 2026 Rob Thornton
// SPDX-License-Identifier: MIT

// Methods on array{T} and map{K: V}, written once in the stdlib against an
// opaque T: a T crosses the call boxed behind a pointer. A body that calls a
// method on a T cannot run that way, so it is specialised per element type
// instead.

#include "ir/codegen.hpp"

#include <algorithm>

namespace saga {

llvm::Value *CodeGen::emit_kind_method_call(const CallExprNode &node,
                                            const std::string &method,
                                            const TypePtr &obj_sem,
                                            llvm::Value *obj) {
  if (auto *spec = kind_method_specialisation(method, obj_sem))
    return emit_kind_specialisation_call(spec, node, obj);
  return emit_opaque_kind_method_call(node, method, obj_sem, obj);
}

// Bindings are keyed by the template's own TypeParam ids so they match the
// node types its eager pass cached.
llvm::Function *CodeGen::kind_method_specialisation(const std::string &method,
                                                    const TypePtr &obj_sem) {
  auto km_decl_it = analyzer.kind_method_decls_.find(obj_sem->kind);
  if (km_decl_it == analyzer.kind_method_decls_.end())
    return nullptr;
  auto m_it = km_decl_it->second.find(method);
  if (m_it == km_decl_it->second.end() ||
      !analyzer.kind_method_uses_typeparam_dispatch_.count(m_it->second.decl))
    return nullptr;

  auto &kmd = m_it->second;
  std::unordered_map<uint32_t, TypePtr> bindings;
  if (obj_sem->kind == TypeKind::Array && !kmd.type_params.empty()) {
    auto &arr = std::get<ArrayTypeInfo>(obj_sem->detail);
    bindings[kmd.type_params[0].id] = arr.element;
  } else if (obj_sem->kind == TypeKind::Map && kmd.type_params.size() >= 2) {
    auto &mp = std::get<MapTypeInfo>(obj_sem->detail);
    bindings[kmd.type_params[0].id] = mp.key;
    bindings[kmd.type_params[1].id] = mp.value;
  }
  return emit_specialisation(*kmd.decl, kmd.original_signature, bindings,
                             find_instantiation(kmd.decl, bindings));
}

llvm::Value *CodeGen::emit_kind_specialisation_call(llvm::Function *spec,
                                                    const CallExprNode &node,
                                                    llvm::Value *obj) {
  std::vector<llvm::Value *> args;
  for (auto &arg_node : node.args)
    if (auto *val = emit_expr(*arg_node))
      args.push_back(val);
  return emit_call(spec, obj, args);
}

llvm::Value *CodeGen::emit_opaque_kind_method_call(const CallExprNode &node,
                                                   const std::string &method,
                                                   const TypePtr &obj_sem,
                                                   llvm::Value *obj) {
  auto km_it = analyzer.kind_methods_.find(obj_sem->kind);
  if (km_it == analyzer.kind_methods_.end())
    return nullptr;
  for (auto &m : km_it->second) {
    if (m.name != method)
      continue;
    auto *callee = kind_method_callee(m, obj_sem, method);
    if (!callee)
      return nullptr;

    auto *result = emit_call(callee, obj, box_kind_method_args(node, m, callee));
    return result ? unbox_kind_method_result(result, m, obj_sem) : nullptr;
  }
  return nullptr;
}

// Defined in this package when the stdlib compiles its own methods; otherwise
// declared against the stdlib package that defines it.
llvm::Function *CodeGen::kind_method_callee(const MethodInfo &m,
                                            const TypePtr &obj_sem,
                                            const std::string &method) {
  const char *km_type_name = nullptr;
  switch (obj_sem->kind) {
  case TypeKind::Array: km_type_name = "Array"; break;
  case TypeKind::Map:   km_type_name = "Map"; break;
  default: break;
  }
  if (!km_type_name)
    return nullptr;

  std::string same_pkg_link =
      mangle(std::string(km_type_name) + "__" + method);
  if (auto *callee = module->getFunction(same_pkg_link))
    return callee;
  if (!m.signature || m.signature->kind != TypeKind::Func)
    return nullptr;

  std::string stdlib_pkg = std::string(km_type_name);
  std::transform(stdlib_pkg.begin(), stdlib_pkg.end(), stdlib_pkg.begin(),
                 ::tolower);
  std::string cross_link =
      mangle(stdlib_pkg, std::string(km_type_name) + "__" + method);
  if (auto *callee = module->getFunction(cross_link))
    return callee;

  return declare_function(
      cross_link, lower_signature(std::get<FuncTypeInfo>(m.signature->detail),
                                  llvm_type(obj_sem)));
}

// A T parameter takes a pointer to the value, so every argument bound to one
// is boxed, a pointer-shaped string or array included. A struct arrives as a
// pointer to its slot, so it is the struct's bytes that are copied.
std::vector<llvm::Value *>
CodeGen::box_kind_method_args(const CallExprNode &node, const MethodInfo &m,
                              llvm::Function *callee) {
  auto *parent_fn = builder.GetInsertBlock()->getParent();
  auto *callee_ft = callee->getFunctionType();
  std::vector<bool> param_is_generic;
  if (m.signature) {
    auto &fi = std::get<FuncTypeInfo>(m.signature->detail);
    for (auto &pt : fi.params)
      param_is_generic.push_back(pt && pt->kind == TypeKind::TypeParam);
  }

  std::vector<llvm::Value *> args;
  for (size_t ai = 0; ai < node.args.size(); ++ai) {
    auto *val = emit_expr(*node.args[ai]);
    if (!val) continue;
    size_t pi = ai + 1; // +1 for self
    bool is_generic = ai < param_is_generic.size() && param_is_generic[ai];
    if (is_generic && pi < callee_ft->getNumParams()) {
      auto arg_sem = semantic_type(*node.args[ai]);
      if (arg_sem && arg_sem->kind == TypeKind::Struct &&
          val->getType()->isPointerTy()) {
        auto *struct_ll = llvm_type(arg_sem);
        auto *tmp = create_entry_alloca(parent_fn, "box.tmp", struct_ll);
        builder.CreateMemCpy(tmp, align_of(struct_ll), val,
                             align_of(struct_ll), size_of(struct_ll));
        val = tmp;
      } else {
        auto *tmp = create_entry_alloca(parent_fn, "box.tmp", val->getType());
        builder.CreateStore(val, tmp);
        val = tmp;
      }
    }
    args.push_back(val);
  }
  return args;
}

// A T result comes back as the address of the runtime's slot for it; the
// value is read out at the concrete element type, pointer-shaped ones too.
llvm::Value *CodeGen::unbox_kind_method_result(llvm::Value *result,
                                               const MethodInfo &m,
                                               const TypePtr &obj_sem) {
  if (!result->getType()->isPointerTy() || !m.signature)
    return result;
  auto &fi = std::get<FuncTypeInfo>(m.signature->detail);
  if (!fi.return_type || fi.return_type->kind != TypeKind::TypeParam)
    return result;

  TypePtr concrete_ret;
  if (obj_sem->kind == TypeKind::Array) {
    concrete_ret = std::get<ArrayTypeInfo>(obj_sem->detail).element;
  } else if (obj_sem->kind == TypeKind::Map) {
    auto &map_info = std::get<MapTypeInfo>(obj_sem->detail);
    auto &tp = std::get<TypeParamInfo>(fi.return_type->detail);
    concrete_ret = tp.param.id == 9991 ? map_info.key : map_info.value;
  }
  if (!concrete_ret)
    return result;
  auto *concrete_ll = llvm_type(concrete_ret);
  if (concrete_ll->isVoidTy())
    return result;
  return builder.CreateLoad(concrete_ll, result, "unbox");
}

} // namespace saga

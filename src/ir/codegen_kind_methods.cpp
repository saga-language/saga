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
  if (auto [spec, concrete] = kind_method_specialisation(method, obj_sem); spec)
    return emit_call(spec, obj,
                     emit_arguments(node,
                                    &std::get<FuncTypeInfo>(concrete->detail),
                                    true));
  return emit_opaque_kind_method_call(node, method, obj_sem, obj);
}

// Bindings are keyed by the template's own TypeParam ids so they match the
// node types its eager pass cached.
std::pair<llvm::Function *, TypePtr>
CodeGen::kind_method_specialisation(const std::string &method,
                                    const TypePtr &obj_sem) {
  auto km_decl_it = analyzer.kind_method_decls_.find(obj_sem->kind);
  if (km_decl_it == analyzer.kind_method_decls_.end())
    return {};
  auto m_it = km_decl_it->second.find(method);
  if (m_it == km_decl_it->second.end() ||
      !analyzer.kind_method_uses_typeparam_dispatch_.count(m_it->second.decl))
    return {};

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
  auto *spec = emit_specialisation(*kmd.decl, kmd.original_signature,
                                   bindings,
                                   find_instantiation(kmd.decl, bindings));
  return {spec, spec ? substitute(kmd.original_signature, bindings) : nullptr};
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

    std::vector<StoredValue> values;
    auto *result = emit_call(callee, obj,
                             box_kind_method_args(node, m, obj_sem, values));
    auto *unboxed =
        result ? unbox_kind_method_result(result, m, obj_sem) : nullptr;
    release_kind_method_args(node, m, obj_sem, values);
    return unboxed;
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

// A T parameter takes a pointer to the value as the collection's slot would
// hold it: a union wrapped, an interface boxed, a pointer-shaped string or
// array behind an address of its own. `values` keeps each T argument for
// settling once the call is done, with a null value for any other.
std::vector<llvm::Value *>
CodeGen::box_kind_method_args(const CallExprNode &node, const MethodInfo &m,
                              const TypePtr &obj_sem,
                              std::vector<StoredValue> &values) {
  const FuncTypeInfo *fi = func_info(m);
  std::vector<llvm::Value *> args;
  for (size_t i = 0; i < node.args.size(); ++i) {
    auto param = fi && i < fi->params.size() ? fi->params[i] : nullptr;
    auto *val = emit_argument(*node.args[i], param, true);
    bool type_param = param && param->kind == TypeKind::TypeParam;
    values.push_back(
        type_param ? stored_value(val, operand_type(*node.args[i]),
                                  kind_slot_type(obj_sem, param))
                   : StoredValue{});
    if (val)
      args.push_back(type_param ? values.back().address : val);
  }
  return args;
}

// The runtime retains a T it keeps and leaves one it only reads, so an
// argument made for the call is released afterwards either way, and a box
// made for it took over the argument.
void CodeGen::release_kind_method_args(const CallExprNode &node,
                                       const MethodInfo &m,
                                       const TypePtr &obj_sem,
                                       const std::vector<StoredValue> &values) {
  const FuncTypeInfo *fi = func_info(m);
  for (size_t i = 0; fi && i < values.size() && i < fi->params.size(); ++i) {
    auto &v = values[i];
    if (!v.val)
      continue;
    auto slot = kind_slot_type(obj_sem, fi->params[i]);
    if (boxes_into(v.val_sem, slot)) {
      retain_if_borrowed(v.val, unwrap_alias(v.val_sem), *node.args[i]);
      emit_release(v.placed, unwrap_alias(slot));
    } else {
      release_handed_over(v.val, *node.args[i], slot);
    }
  }
}

// The element, key or value type a collection method's T, K or V stands for.
TypePtr CodeGen::kind_slot_type(const TypePtr &obj_sem, const TypePtr &param) {
  if (obj_sem->kind == TypeKind::Array)
    return std::get<ArrayTypeInfo>(obj_sem->detail).element;
  if (obj_sem->kind != TypeKind::Map)
    return nullptr;
  auto &map_info = std::get<MapTypeInfo>(obj_sem->detail);
  auto &tp = std::get<TypeParamInfo>(param->detail);
  return tp.param.id == 9991 ? map_info.key : map_info.value;
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

  auto concrete_ret = kind_slot_type(obj_sem, fi.return_type);
  if (!concrete_ret)
    return result;
  auto *concrete_ll = llvm_type(concrete_ret);
  if (concrete_ll->isVoidTy())
    return result;
  return builder.CreateLoad(concrete_ll, result, "unbox");
}

} // namespace saga

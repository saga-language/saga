// Copyright 2026 Rob Thornton
// SPDX-License-Identifier: MIT

// Methods on the scalar intrinsic types (int, float, bool, string), which
// the stdlib defines in a package per type, and the per-member callee lookup
// union dispatch shares with them.

#include "ir/codegen.hpp"

namespace saga {

// Canonical (capitalized, width-bearing) name for a scalar intrinsic type, used
// to build receiver-method link names (`pkg__TypeName__Method`). It must match
// the declaration side (codegen_decls `intrinsic_internal_name`) and `.sgi`.
// `type_to_string` is now the lowercase *source* spelling, so it can't serve.
static std::string scalar_intrinsic_mangle_name(const TypePtr &t) {
  switch (t->kind) {
  case TypeKind::Bool:
    return "Bool";
  case TypeKind::String:
    return "String";
  case TypeKind::Float: {
    auto bits = std::get<FloatType>(t->detail).bits;
    return bits == 0 ? "Float" : "Float" + std::to_string(bits);
  }
  case TypeKind::Int: {
    auto &i = std::get<IntType>(t->detail);
    if (i.bits == 0)
      return i.is_signed ? "Int" : "Byte";
    return (i.is_signed ? "Int" : "Uint") + std::to_string(i.bits);
  }
  default:
    return type_to_string(t);
  }
}

llvm::Value *CodeGen::emit_scalar_method_call(const CallExprNode &node,
                                              const std::string &method,
                                              const TypePtr &obj_sem,
                                              llvm::Value *obj) {
  const FuncTypeInfo *m_fi = nullptr;
  auto *callee = resolve_member_method_callee(obj_sem, method, &m_fi);
  if (!callee)
    return nullptr;
  std::vector<llvm::Value *> arg_vals;
  for (auto &arg_node : node.args)
    if (auto *val = emit_expr(*arg_node))
      arg_vals.push_back(val);
  return emit_call(callee, obj, arg_vals);
}

llvm::Function *CodeGen::resolve_member_method_callee(
    const TypePtr &member_sem, const std::string &method,
    const FuncTypeInfo **out_sig) {
  *out_sig = nullptr;
  if (!member_sem)
    return nullptr;

  if (member_sem->kind == TypeKind::Struct) {
    auto &info = std::get<StructTypeInfo>(member_sem->detail);
    for (auto &m : info.methods) {
      if (m.name != method || !m.signature ||
          m.signature->kind != TypeKind::Func)
        continue;
      auto &fi = std::get<FuncTypeInfo>(m.signature->detail);
      *out_sig = &fi;
      std::string origin =
          info.origin_package.empty() ? package_name : info.origin_package;
      std::string link = mangle(origin, info.name + "__" + method);
      return forward_declare_method(link, fi);
    }
    return nullptr;
  }

  const Type *canonical = nullptr;
  const char *pkg = nullptr;
  switch (member_sem->kind) {
  case TypeKind::Int: {
    auto &ii = std::get<IntType>(member_sem->detail);
    pkg = "int";
    if (ii.bits == 0)
      canonical = analyzer.builtins.int_type.get();
    else if (ii.is_signed)
      switch (ii.bits) {
      case 8:  canonical = analyzer.builtins.int8_type.get();  break;
      case 16: canonical = analyzer.builtins.int16_type.get(); break;
      case 32: canonical = analyzer.builtins.int32_type.get(); break;
      case 64: canonical = analyzer.builtins.int64_type.get(); break;
      }
    else
      switch (ii.bits) {
      case 8:  canonical = analyzer.builtins.uint8_type.get();  break;
      case 16: canonical = analyzer.builtins.uint16_type.get(); break;
      case 32: canonical = analyzer.builtins.uint32_type.get(); break;
      case 64: canonical = analyzer.builtins.uint64_type.get(); break;
      }
    break;
  }
  case TypeKind::Float:  canonical = analyzer.builtins.float_type.get();  pkg = "float";  break;
  case TypeKind::Bool:   canonical = analyzer.builtins.bool_type.get();   pkg = "bool";   break;
  case TypeKind::String: canonical = analyzer.builtins.string_type.get(); pkg = "string"; break;
  default: return nullptr;
  }

  auto tm_it = analyzer.type_methods_.find(member_sem.get());
  if (tm_it == analyzer.type_methods_.end() && canonical &&
      canonical != member_sem.get())
    tm_it = analyzer.type_methods_.find(canonical);
  if (tm_it == analyzer.type_methods_.end())
    return nullptr;
  for (auto &m : tm_it->second) {
    if (m.name != method || !m.signature ||
        m.signature->kind != TypeKind::Func)
      continue;
    auto &fi = std::get<FuncTypeInfo>(m.signature->detail);
    *out_sig = &fi;
    std::string tn = scalar_intrinsic_mangle_name(member_sem);
    if (auto *fn = module->getFunction(mangle(tn + "__" + method)))
      return fn;
    std::string cross = mangle(pkg, tn + "__" + method);
    if (auto *fn = module->getFunction(cross))
      return fn;
    return declare_function(cross, lower_signature(fi, llvm_type(member_sem)));
  }
  return nullptr;
}

} // namespace saga

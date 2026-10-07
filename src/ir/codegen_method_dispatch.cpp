// Copyright 2026 Rob Thornton
// SPDX-License-Identifier: MIT

// Calls whose callee is a selector, `obj.method(args)`: routed by what the
// object is to a module function, a type's function, or a method on the
// receiver's kind. Direct (identifier) calls live in codegen_calls.cpp.

#include "ir/codegen.hpp"

namespace saga {

// A method that writes through its receiver needs somewhere for the write to
// land that no second name can see, so the receiver is made unique before the
// callee gets it. The callee takes the collection by value and cannot rebind
// the caller's slot; only this side knows which slot that is.
llvm::Value *CodeGen::emit_receiver(const Node &object, const TypePtr &obj_sem,
                                    const std::string &method) {
  // The shape, not the declared type: an alias of a collection reaches the
  // same runtime buffer and needs the same barrier.
  auto shape = unwrap_alias(obj_sem);
  if (!shape || !kind_method_mutates(shape, method))
    return emit_borrowed(object);
  return make_binding_unique(object, shape);
}

bool CodeGen::kind_method_mutates(const TypePtr &shape,
                                  const std::string &method) {
  auto kind_it = analyzer.kind_method_decls_.find(shape->kind);
  if (kind_it == analyzer.kind_method_decls_.end())
    return false;
  auto m_it = kind_it->second.find(method);
  return m_it != kind_it->second.end() && m_it->second.mutates_receiver;
}

llvm::Value *CodeGen::emit_method_or_module_call(const CallExprNode &node,
                                                 const Node &parent) {
  auto &sel = std::get<SelectorNode>(node.callee->data);
  std::string method(sel.field.name);

  // declared_type, not semantic_type: an alias-bound method is found by the
  // alias's own name, which is the one question lowering cannot answer from
  // the shape.
  auto obj_sem = declared_type(*sel.object);
  if (auto result = emit_alias_method_call(node, sel, method, obj_sem))
    return *result;
  obj_sem = unwrap_alias(obj_sem);

  if (obj_sem && obj_sem->kind == TypeKind::Module)
    return emit_module_function_call(node, method, obj_sem);
  // A bare type name as the object names a function on the type, not a value.
  if (obj_sem && obj_sem->kind == TypeKind::Struct) {
    auto *osym = node_symbol(*sel.object);
    if (osym && osym->kind == SymbolKind::Type)
      return emit_type_method_call(node, method, obj_sem);
    if (auto result = emit_field_function_call(node, sel, method, obj_sem))
      return *result;
  }

  auto *obj = emit_receiver(*sel.object, obj_sem, method);
  if (!obj || !obj_sem)
    return nullptr;
  switch (obj_sem->kind) {
  case TypeKind::Union:
    return emit_union_method_dispatch(node, method, obj_sem, obj);
  case TypeKind::Interface:
    return emit_interface_dispatch(node, sel, method, obj_sem, obj);
  case TypeKind::Enum:
    return emit_enum_method_call(node, method, obj_sem, obj);
  case TypeKind::Array:
  case TypeKind::Map:
    return emit_kind_method_call(node, method, obj_sem, obj);
  case TypeKind::Struct:
    return emit_struct_method_call(node, sel, method, obj_sem, obj, parent);
  default:
    return emit_scalar_method_call(node, method, obj_sem, obj);
  }
}

// A method bound to the alias itself (`fn (u UserID) Display()`) is found by
// the alias's name; anything else falls through to the underlying type, so
// `uid.String()` still reaches int's.
std::optional<llvm::Value *>
CodeGen::emit_alias_method_call(const CallExprNode &node,
                                const SelectorNode &sel,
                                const std::string &method,
                                const TypePtr &obj_sem) {
  if (!obj_sem || obj_sem->kind != TypeKind::Alias)
    return std::nullopt;
  auto &ai = std::get<AliasTypeInfo>(obj_sem->detail);
  for (auto &m : ai.methods) {
    if (m.name != method) continue;
    std::string link_name = mangle(type_to_string(obj_sem) + "__" + method);
    auto *callee = module->getFunction(link_name);
    if (!callee)
      return std::nullopt;
    auto *self = emit_borrowed(*sel.object);
    return emit_call(callee, self,
                     emit_arguments(node, method_signature(ai.methods, method),
                                    true));
  }
  return std::nullopt;
}

llvm::Value *CodeGen::emit_type_method_call(const CallExprNode &node,
                                            const std::string &method,
                                            const TypePtr &obj_sem) {
  auto &sinfo = std::get<StructTypeInfo>(obj_sem->detail);
  auto *callee = module->getFunction(mangle(sinfo.name + "__" + method));
  if (!callee)
    return nullptr;
  auto func_type = semantic_type(*node.callee);
  if (!func_type || func_type->kind != TypeKind::Func)
    return nullptr;
  return emit_resolved_call(callee, func_type, node);
}

llvm::Value *CodeGen::emit_module_function_call(const CallExprNode &node,
                                                const std::string &method,
                                                const TypePtr &obj_sem) {
  auto &mod = std::get<ModuleTypeInfo>(obj_sem->detail);
  TypePtr func_type;
  for (auto &exp : mod.exports) {
    if (exp.name == method) {
      func_type = exp.type;
      break;
    }
  }
  if (!func_type || func_type->kind != TypeKind::Func)
    return nullptr;

  auto *callee = declare_import(mod.name, method, func_type);
  if (!callee)
    return nullptr;

  return emit_resolved_call(callee, func_type, node);
}

} // namespace saga

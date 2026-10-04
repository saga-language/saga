// Copyright 2026 Rob Thornton
// SPDX-License-Identifier: MIT

// Calls on a struct value: a function stored in one of its fields, or a
// method declared on it or on a struct it embeds.

#include "ir/codegen.hpp"

namespace saga {

std::optional<llvm::Value *>
CodeGen::emit_field_function_call(const CallExprNode &node,
                                  const SelectorNode &sel,
                                  const std::string &method,
                                  const TypePtr &obj_sem) {
  auto &sinfo = std::get<StructTypeInfo>(obj_sem->detail);
  for (auto &fld : sinfo.fields) {
    if (fld.name != method) continue;
    if (!fld.type || fld.type->kind != TypeKind::Func) break;

    // Resolve the struct ptr — prefer the alloca for a local identifier
    // so we can GEP without extracting from a loaded value.
    auto *ptr_type = llvm::PointerType::getUnqual(context);
    llvm::Value *struct_ptr = nullptr;
    if (auto *id = std::get_if<IdentifierNode>(&sel.object->data)) {
      auto lit = locals.find(std::string(id->name));
      if (lit != locals.end()) {
        auto *alloca = lit->second;
        auto st_it = struct_types.find(struct_cache_key(sinfo));
        if (st_it != struct_types.end() &&
            alloca->getAllocatedType() == st_it->second) {
          struct_ptr = alloca;
        } else if (alloca->getAllocatedType()->isPointerTy()) {
          struct_ptr = builder.CreateLoad(ptr_type, alloca, "obj.load");
        }
      }
    }
    if (!struct_ptr)
      struct_ptr = emit_expr(*sel.object);
    if (!struct_ptr) return nullptr;

    auto [gep, ftype] = struct_field_gep(struct_ptr, obj_sem, method);
    if (!gep) break;
    auto *fn_ptr = builder.CreateLoad(ptr_type, gep, "field.fn");

    std::vector<llvm::Value *> args;
    std::vector<llvm::Type *> param_types;
    auto &fi = std::get<FuncTypeInfo>(fld.type->detail);
    for (auto &pt : fi.params)
      param_types.push_back(llvm_type(pt));
    for (auto &arg_node : node.args) {
      auto *val = emit_expr(*arg_node);
      if (val) args.push_back(val);
    }
    llvm::Type *ret_ll =
        !fi.return_type ? void_ll_type : llvm_type(fi.return_type);
    auto *fn_type = llvm::FunctionType::get(ret_ll, param_types, false);
    if (ret_ll->isVoidTy()) {
      builder.CreateCall(fn_type, fn_ptr, args);
      return nullptr;
    }
    return builder.CreateCall(fn_type, fn_ptr, args, "field.call");
  }
  return std::nullopt;
}

// Task and Context are structs to the checker; their built-in methods are
// tried before any the program declared.
llvm::Value *CodeGen::emit_struct_method_call(const CallExprNode &node,
                                              const SelectorNode &sel,
                                              const std::string &method,
                                              const TypePtr &obj_sem,
                                              llvm::Value *obj,
                                              const Node &parent) {
  auto &sinfo = std::get<StructTypeInfo>(obj_sem->detail);
  if (sinfo.name == "Task")
    if (auto result = emit_task_method_call(method, sinfo, obj))
      return *result;
  if (sinfo.name == "Context")
    if (auto result = emit_context_method_call(node, method, obj))
      return *result;

  auto [recv, recv_sem] = embedded_method_target(sel, method, obj_sem, obj);
  if (auto *spec = generic_method_specialisation(method, recv_sem, parent))
    return emit_generic_method_call(spec, node, sel, recv);
  return emit_declared_method_call(node, sel, method, recv_sem, recv);
}

// A method the struct does not declare itself lives on a struct it embeds,
// possibly several levels down: the receiver becomes that embedded value and
// the call is formed from its struct's side.
std::pair<llvm::Value *, TypePtr>
CodeGen::embedded_method_target(const SelectorNode &sel,
                                const std::string &method,
                                const TypePtr &obj_sem, llvm::Value *obj) {
  auto &outer = std::get<StructTypeInfo>(obj_sem->detail);
  for (auto &m : outer.methods)
    if (m.name == method)
      return {obj, obj_sem};
  auto st_it = struct_types.find(struct_cache_key(outer));
  if (st_it == struct_types.end())
    return {obj, obj_sem};
  auto *outer_st = st_it->second;

  // Pointer to outer struct: prefer the local alloca for an identifier;
  // otherwise spill the SSA struct value.
  llvm::Value *outer_ptr = nullptr;
  if (auto *id = std::get_if<IdentifierNode>(&sel.object->data)) {
    auto local_it = locals.find(std::string(id->name));
    if (local_it != locals.end() &&
        local_it->second->getAllocatedType() == outer_st)
      outer_ptr = local_it->second;
  }
  if (!outer_ptr) {
    if (obj->getType()->isPointerTy()) {
      outer_ptr = obj;
    } else {
      auto *parent_fn = builder.GetInsertBlock()->getParent();
      auto *tmp = create_entry_alloca(parent_fn, "embed.self.spill", outer_st);
      builder.CreateStore(obj, tmp);
      outer_ptr = tmp;
    }
  }

  auto [recv, recv_sem] = embed_method_receiver(outer_ptr, obj_sem, method);
  if (!recv)
    return {obj, obj_sem};
  return {recv, recv_sem};
}

llvm::Function *
CodeGen::generic_method_specialisation(const std::string &method,
                                       const TypePtr &obj_sem,
                                       const Node &parent) {
  auto &info = std::get<StructTypeInfo>(obj_sem->detail);
  for (auto &m : info.methods) {
    if (m.name != method || !m.signature || !has_type_params(m.signature))
      continue;
    auto fd_it = analyzer.func_decl_by_type_.find(m.signature.get());
    auto *bindings = node_type_args_of(parent);
    if (fd_it == analyzer.func_decl_by_type_.end() || !bindings)
      return nullptr;
    return emit_specialisation(*fd_it->second, m.signature, *bindings,
                               find_instantiation(fd_it->second, *bindings));
  }
  return nullptr;
}

llvm::Value *CodeGen::emit_generic_method_call(llvm::Function *spec,
                                               const CallExprNode &node,
                                               const SelectorNode &sel,
                                               llvm::Value *obj) {
  std::vector<llvm::Value *> args;
  // self pointer
  if (auto *id = std::get_if<IdentifierNode>(&sel.object->data)) {
    auto local_it = locals.find(std::string(id->name));
    if (local_it != locals.end())
      args.push_back(local_it->second);
    else
      args.push_back(obj);
  } else {
    args.push_back(obj);
  }
  for (auto &a : node.args) {
    auto *v = emit_expr(*a);
    if (v) args.push_back(v);
  }
  if (spec->getReturnType()->isVoidTy()) {
    builder.CreateCall(spec, args);
    return nullptr;
  }
  return builder.CreateCall(spec, args, "gen.mcall");
}

llvm::Value *CodeGen::emit_declared_method_call(const CallExprNode &node,
                                                const SelectorNode &sel,
                                                const std::string &method,
                                                const TypePtr &obj_sem,
                                                llvm::Value *obj) {
  auto &info = std::get<StructTypeInfo>(obj_sem->detail);
  auto *callee = struct_method_callee(info, method);
  if (!callee)
    return nullptr;

  // Self is a pointer to the struct.  Resolve through the parameterized
  // cache key so a generic instantiation (e.g. `lib__Box<Int>`) matches
  // its actual LLVM struct type, not the unparameterized base name.
  std::string self_struct_key = struct_cache_key(info);
  llvm::Value *self_ptr = obj;
  if (auto *id = std::get_if<IdentifierNode>(&sel.object->data)) {
    auto local_it = locals.find(std::string(id->name));
    if (local_it != locals.end()) {
      auto *alloca = local_it->second;
      auto *alloca_type = alloca->getAllocatedType();
      auto st_it = struct_types.find(self_struct_key);
      if (st_it != struct_types.end() && alloca_type == st_it->second) {
        self_ptr = alloca;
      } else if (alloca_type->isPointerTy()) {
        self_ptr = builder.CreateLoad(alloca_type, alloca, "self.ptr");
      }
    }
  }

  const FuncTypeInfo *m_fi = nullptr;
  for (auto &m : info.methods) {
    if (m.name == method && m.signature &&
        m.signature->kind == TypeKind::Func) {
      m_fi = &std::get<FuncTypeInfo>(m.signature->detail);
      break;
    }
  }

  std::vector<llvm::Value *> arg_vals;
  for (auto &arg_node : node.args) {
    if (auto *val = emit_expr(*arg_node))
      arg_vals.push_back(val);
  }
  return emit_receiver_call(callee, obj_sem, self_ptr, arg_vals, m_fi);
}

llvm::Function *CodeGen::struct_method_callee(const StructTypeInfo &info,
                                              const std::string &method) {
  std::string struct_origin =
      info.origin_package.empty() ? package_name : info.origin_package;
  std::string link_name = mangle(struct_origin, info.name + "__" + method);
  if (auto *callee = module->getFunction(link_name))
    return callee;

  // A method on a generic struct has no eagerly-declared symbol — there is
  // no single concrete signature to declare — so it is monomorphised here,
  // at the first call that fixes the receiver's type arguments.
  if (!info.type_args.empty() && !info.type_params.empty())
    if (auto *callee = emit_generic_method(info, struct_origin, method))
      return callee;

  // A local type's methods are declared up front and an imported one's when
  // its package is materialized; this covers a type reached neither way.
  for (auto &m : info.methods)
    if (m.name == method && m.signature &&
        m.signature->kind == TypeKind::Func)
      return forward_declare_method(
          link_name, std::get<FuncTypeInfo>(m.signature->detail));
  return nullptr;
}

} // namespace saga

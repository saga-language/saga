// Copyright 2026 Rob Thornton
// SPDX-License-Identifier: MIT

// Type mangling, generic specialisation emission, and the
// FuncEmissionScope RAII used to swap codegen state for a fresh function.

#include "ir/codegen.hpp"

#include <llvm/IR/Constants.h>

namespace saga {

// ---------------------------------------------------------------------------
// Step 5a — FuncEmissionScope (RAII)
// ---------------------------------------------------------------------------

CodeGen::FuncEmissionScope::FuncEmissionScope(CodeGen &cg) : cg_(cg) {
  saved_bb_ = cg.builder.GetInsertBlock();
  if (saved_bb_)
    saved_ip_ = cg.builder.GetInsertPoint();
  saved_locals_ = std::move(cg.locals);
  saved_managed_locals_ = std::move(cg.managed_locals);
  saved_loop_stack_ = std::move(cg.loop_stack);
  saved_current_func_is_main_ = cg.current_func_is_main;
  saved_current_instantiation_ = cg.current_instantiation_;
  saved_current_actor_ = cg.current_actor;
  saved_pending_channel_alloca_ = cg.pending_channel_alloca_;

  // Reset to fresh-function defaults.
  cg.locals.clear();
  cg.managed_locals.clear();
  cg.loop_stack.clear();
  cg.current_func_is_main = false;
  cg.current_instantiation_ = nullptr;
  cg.current_actor = nullptr;
  cg.pending_channel_alloca_ = nullptr;
}

CodeGen::FuncEmissionScope::~FuncEmissionScope() {
  cg_.locals = std::move(saved_locals_);
  cg_.managed_locals = std::move(saved_managed_locals_);
  cg_.loop_stack = std::move(saved_loop_stack_);
  cg_.current_func_is_main = saved_current_func_is_main_;
  cg_.current_instantiation_ = saved_current_instantiation_;
  cg_.current_actor = saved_current_actor_;
  cg_.pending_channel_alloca_ = saved_pending_channel_alloca_;
  if (saved_bb_) {
    cg_.builder.SetInsertPoint(saved_bb_, saved_ip_);
  } else {
    cg_.builder.ClearInsertionPoint();
  }
}

// ---------------------------------------------------------------------------
// Step 5 — linker-safe type mangler
// ---------------------------------------------------------------------------

std::string CodeGen::mangle_type(const TypePtr &t) const {
  if (!t)
    return "err";
  switch (t->kind) {
  case TypeKind::Int:    return "Int";
  case TypeKind::Float:  return "Float";
  case TypeKind::Bool:   return "Bool";
  case TypeKind::String: return "String";
  case TypeKind::Void:   return "Void";
  case TypeKind::Array: {
    auto &ai = std::get<ArrayTypeInfo>(t->detail);
    return "Arr_" + mangle_type(ai.element) + "_End";
  }
  case TypeKind::Map: {
    auto &mi = std::get<MapTypeInfo>(t->detail);
    return "Map_" + mangle_type(mi.key) + "_" + mangle_type(mi.value) + "_End";
  }
  case TypeKind::Union: {
    auto &ui = std::get<UnionTypeInfo>(t->detail);
    std::string out = "Un_";
    for (size_t i = 0; i < ui.alternatives.size(); ++i) {
      if (i) out += "_";
      out += mangle_type(ui.alternatives[i]);
    }
    out += "_End";
    return out;
  }
  case TypeKind::Func: {
    auto &fi = std::get<FuncTypeInfo>(t->detail);
    std::string out = "Fn_";
    for (size_t i = 0; i < fi.params.size(); ++i) {
      if (i) out += "_";
      out += mangle_type(fi.params[i]);
    }
    out += "_to_";
    if (!fi.return_type)
      out += "Void";
    else
      out += mangle_type(fi.return_type);
    out += "_End";
    return out;
  }
  case TypeKind::Struct: {
    auto &si = std::get<StructTypeInfo>(t->detail);
    std::string pkg = package_name.empty() ? "local" : package_name;
    std::string out = "pkg_" + pkg + "_" + si.name;
    // Without the arguments, Box<int> and Box<string> name one symbol.
    for (auto &a : si.type_args)
      out += "_" + mangle_type(a);
    return out;
  }
  case TypeKind::Alias: {
    auto &ai = std::get<AliasTypeInfo>(t->detail);
    std::string pkg = package_name.empty() ? "local" : package_name;
    return "pkg_" + pkg + "_" + ai.name;
  }
  case TypeKind::Enum: {
    auto &ei = std::get<EnumTypeInfo>(t->detail);
    std::string pkg = package_name.empty() ? "local" : package_name;
    return "pkg_" + pkg + "_" + ei.name;
  }
  case TypeKind::Interface: {
    auto &ii = std::get<InterfaceTypeInfo>(t->detail);
    std::string pkg = package_name.empty() ? "local" : package_name;
    return "If_" + pkg + "_" + ii.name;
  }
  case TypeKind::TypeParam: {
    auto &tpi = std::get<TypeParamInfo>(t->detail);
    return "Tp_" + tpi.param.name; // should not survive into specialisation
  }
  default:
    return "Unk";
  }
}

std::string CodeGen::mangle_specialisation(
    const FuncDeclNode &fn,
    const std::vector<TypePtr> &ordered_type_args) const {
  // For receiver methods, include the receiver struct's name so two
  // generic methods with the same simple name on different structs do
  // not collide on a single LinkOnceODR symbol (e.g. Box.Get vs
  // Container.Get specialised to the same type argument).
  std::string base(fn.name.name);
  if (fn.receiver) {
    std::string recv_name;
    const auto &type_node = *fn.receiver->type;
    if (auto *gt = std::get_if<GenericTypeAppNode>(&type_node.data)) {
      if (gt->base_type)
        if (auto *id = std::get_if<IdentifierNode>(&gt->base_type->data))
          recv_name = std::string(id->name);
    } else if (auto *id = std::get_if<IdentifierNode>(&type_node.data)) {
      recv_name = std::string(id->name);
    }
    if (!recv_name.empty()) base = recv_name + "__" + base;
  }
  std::string out = "gen__" + mangle(base);
  for (auto &t : ordered_type_args) {
    out += "__" + mangle_type(t);
  }
  return out;
}

// ---------------------------------------------------------------------------
// Step 5b — emit a monomorphised specialisation
// ---------------------------------------------------------------------------

llvm::Function *CodeGen::emit_specialisation(
    const FuncDeclNode &fn, const TypePtr &generic_fn_type,
    const std::unordered_map<uint32_t, TypePtr> &bindings,
    const Analyzer::BodyInstantiation *inst) {
  // Compute ordered type-argument list (declaration order) so the
  // specialised link name is deterministic across packages.
  std::vector<TypePtr> ordered_args;
  auto tpl_it = analyzer.generic_templates_.find(&fn);
  if (tpl_it != analyzer.generic_templates_.end()) {
    for (auto &tp : tpl_it->second.type_params) {
      auto it = bindings.find(tp.id);
      if (it != bindings.end())
        ordered_args.push_back(it->second);
    }
  }

  std::string mangled = mangle_specialisation(fn, ordered_args);
  if (auto *existing = module->getFunction(mangled))
    return existing;
  if (inst && inst->failed)
    return nullptr;

  auto concrete = substitute(generic_fn_type, bindings);
  auto &fi = std::get<FuncTypeInfo>(concrete->detail);

  // Declared like any function; a receiver leads as the struct's address.
  bool has_receiver = fn.receiver.has_value();
  auto *ptr_ty = llvm::PointerType::getUnqual(context);
  auto sig = lower_signature(fi, has_receiver ? ptr_ty : nullptr);
  auto *func = declare_function(mangled, sig,
                                llvm::Function::LinkOnceODRLinkage);
  name_params(func, sig, fn);
  return_sems_[func] = fi.return_type;

  FuncEmissionScope guard(*this);
  current_instantiation_ = inst;
  if (has_receiver)
    emit_receiver_method_body(fn, func, fi);
  else
    emit_function_body_inner(fn, func, fi, /*is_main=*/false);
  return func;
}

llvm::Function *CodeGen::emit_generic_method(const StructTypeInfo &info,
                                             const std::string &origin,
                                             const std::string &method) {
  auto tpl = origin == package_name
                 ? analyzer.generic_method_decl(info.name, method,
                                                info.type_args)
                 : analyzer.load_imported_method_decl(origin, info.name, method,
                                                      info.type_args);
  if (!tpl.decl || !tpl.template_signature) return nullptr;
  return emit_specialisation(*tpl.decl, tpl.template_signature, tpl.bindings,
                             tpl.instantiation);
}

} // namespace saga

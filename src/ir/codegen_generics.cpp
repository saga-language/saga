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
    if (fi.returns.empty())
      out += "Void";
    else
      out += mangle_type(fi.returns[0]);
    out += "_End";
    return out;
  }
  case TypeKind::Struct: {
    auto &si = std::get<StructTypeInfo>(t->detail);
    std::string pkg = package_name.empty() ? "local" : package_name;
    return "pkg_" + pkg + "_" + si.name;
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

  // Concrete signature from bindings.  Struct and interface params are
  // pointers in LLVM function signatures (matching resolve_type_node).
  auto concrete = substitute(generic_fn_type, bindings);
  auto &fi = std::get<FuncTypeInfo>(concrete->detail);

  auto to_param_ll = [&](const TypePtr &pt) -> llvm::Type * {
    if (pt && pt->kind == TypeKind::Struct)
      return llvm::PointerType::getUnqual(context);
    return llvm_type(pt);
  };

  std::vector<llvm::Type *> param_ll;
  bool has_receiver = fn.receiver.has_value();
  std::string recv_struct_name;
  if (has_receiver) {
    param_ll.push_back(llvm::PointerType::getUnqual(context));
    if (auto *ri = std::get_if<IdentifierNode>(&fn.receiver->type->data))
      recv_struct_name = std::string(ri->name);
  }
  for (auto &pt : fi.params) {
    auto *ll = to_param_ll(pt);
    if (!ll) return nullptr;
    param_ll.push_back(ll);
  }
  llvm::Type *ret_ll = void_ll_type;
  if (fi.returns.size() == 1) {
    ret_ll = to_param_ll(fi.returns[0]);
    if (!ret_ll) return nullptr;
  } else if (fi.returns.size() > 1) {
    std::vector<llvm::Type *> rfs;
    for (auto &r : fi.returns) rfs.push_back(to_param_ll(r));
    auto *st =
        llvm::StructType::create(context, rfs, "saga.ret." + mangled);
    multi_return_types[mangled] = st;
    multi_return_counts[mangled] = fi.returns.size();
    ret_ll = st;
  }

  auto *ft = llvm::FunctionType::get(ret_ll, param_ll, /*isVarArg=*/false);
  auto *func = llvm::Function::Create(
      ft, llvm::Function::LinkOnceODRLinkage, mangled, module.get());

  // Name the arguments for IR readability.
  size_t arg_idx = 0;
  if (has_receiver) {
    func->getArg(arg_idx++)->setName(
        std::string(fn.receiver->name.name));
  }
  for (auto &param : fn.signature.params) {
    for (auto &ident : param.names.identifiers) {
      if (arg_idx < func->arg_size())
        func->getArg(arg_idx++)->setName(std::string(ident.name));
    }
  }

  // Emit the body under a fresh per-function scope.
  {
    FuncEmissionScope guard(*this);
    current_instantiation_ = inst;

    if (has_receiver) {
      // Receiver method: set up self, struct fields, and params manually
      // (emit_function_body_inner doesn't handle receivers).
      auto *entry = llvm::BasicBlock::Create(context, "entry", func);
      builder.SetInsertPoint(entry);
      locals.clear();
      managed_locals.clear();
      current_func_is_main = false;

      // Self parameter.
      std::string recv_name(fn.receiver->name.name);
      auto *self_alloca = create_entry_alloca(
          func, recv_name, llvm::PointerType::getUnqual(context));
      builder.CreateStore(func->getArg(0), self_alloca);
      locals[recv_name] = self_alloca;

      // Inject struct fields as locals.
      if (!recv_struct_name.empty()) {
        std::string recv_key = mangle(package_name, recv_struct_name);
        auto st_it = struct_types.find(recv_key);
        if (st_it != struct_types.end()) {
          auto *st = st_it->second;
          auto &fields = struct_fields[recv_key];
          auto *self_ptr = builder.CreateLoad(
              llvm::PointerType::getUnqual(context), self_alloca, "self.ptr");
          for (size_t fi2 = 0; fi2 < fields.size(); ++fi2) {
            auto *ftype = st->getElementType(fi2);
            auto *gep = builder.CreateStructGEP(
                st, self_ptr, fi2, fields[fi2]);
            auto *val = builder.CreateLoad(
                ftype, gep, fields[fi2] + ".val");
            auto *field_alloca = create_entry_alloca(
                func, fields[fi2], ftype);
            builder.CreateStore(val, field_alloca);
            locals[fields[fi2]] = field_alloca;
          }
        }
      }

      // Regular parameters (with concrete types from substitution).
      size_t pidx = 1; // skip self
      size_t ll_idx = 1;
      for (auto &param : fn.signature.params) {
        for (auto &ident : param.names.identifiers) {
          auto *ll_type = ll_idx < param_ll.size()
                              ? param_ll[ll_idx]
                              : llvm::PointerType::getUnqual(context);
          std::string pname(ident.name);
          auto *alloca = create_entry_alloca(func, pname, ll_type);
          builder.CreateStore(func->getArg(pidx++), alloca);
          locals[pname] = alloca;
          ++ll_idx;
        }
      }

      // Emit body.
      auto &block = std::get<BlockNode>(fn.body->data);
      auto *tail_val = emit_block(block);

      if (!builder.GetInsertBlock()->getTerminator()) {
        emit_release_locals();
        auto *ret_type = func->getReturnType();
        if (ret_type->isVoidTy()) {
          builder.CreateRetVoid();
        } else if (tail_val && tail_val->getType() == ret_type) {
          builder.CreateRet(tail_val);
        } else {
          builder.CreateRet(llvm::Constant::getNullValue(ret_type));
        }
      }
    } else {
      emit_function_body_inner(fn, func, param_ll, /*is_main=*/false);
    }
  }
  return func;
}

} // namespace saga

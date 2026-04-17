// Copyright 2026 Rob Thornton
// SPDX-License-Identifier: MIT

#include "ir/codegen.hpp"

#include <algorithm>
#include <llvm/IR/Constants.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/InlineAsm.h>
#include <llvm/IR/Verifier.h>

namespace mc {

// Parse an integer literal (decimal only; for intrinsic argument indices).
static int64_t parse_int_literal(std::string_view lit) {
  int64_t val = 0;
  for (char c : lit)
    if (c != '_')
      val = val * 10 + (c - '0');
  return val;
}

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
  saved_current_func_return_sem_ = cg.current_func_return_sem;
  saved_current_instantiation_ = cg.current_instantiation_;
  saved_current_actor_ = cg.current_actor;
  saved_pending_channel_alloca_ = cg.pending_channel_alloca_;

  // Reset to fresh-function defaults.
  cg.locals.clear();
  cg.managed_locals.clear();
  cg.loop_stack.clear();
  cg.current_func_is_main = false;
  cg.current_func_return_sem = nullptr;
  cg.current_instantiation_ = nullptr;
  cg.current_actor = nullptr;
  cg.pending_channel_alloca_ = nullptr;
}

CodeGen::FuncEmissionScope::~FuncEmissionScope() {
  cg_.locals = std::move(saved_locals_);
  cg_.managed_locals = std::move(saved_managed_locals_);
  cg_.loop_stack = std::move(saved_loop_stack_);
  cg_.current_func_is_main = saved_current_func_is_main_;
  cg_.current_func_return_sem = saved_current_func_return_sem_;
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
  std::string base = mangle(std::string(fn.name.name));
  std::string out = "gen__" + base;
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
        llvm::StructType::create(context, rfs, "mc.ret." + mangled);
    multi_return_types[mangled] = st;
    multi_return_counts[mangled] = fi.returns.size();
    ret_ll = st;
  }

  auto *ft = llvm::FunctionType::get(ret_ll, param_ll, /*isVarArg=*/false);
  auto *func = llvm::Function::Create(
      ft, llvm::Function::LinkOnceODRLinkage, mangled, module.get());

  // Name the arguments for IR readability.
  size_t arg_idx = 0;
  for (auto &param : fn.signature.params) {
    for (auto &ident : param.names.identifiers) {
      if (arg_idx < func->arg_size())
        func->getArg(arg_idx++)->setName(std::string(ident.name));
    }
  }

  // Emit the body under a fresh per-function scope.  The scope saves the
  // caller's state (including builder insert point, locals, loop stack,
  // and current_instantiation_) so a specialisation emitted mid-call
  // doesn't leak state back to its caller.
  {
    FuncEmissionScope guard(*this);
    current_instantiation_ = inst;
    emit_function_body_inner(fn, func, param_ll, /*is_main=*/false);
  }
  return func;
}

llvm::Value *CodeGen::emit_selector(const SelectorNode &node,
                                    const Node &parent) {
  std::string field_name(node.field.name);

  // Module selector: mod.Symbol — used for constants, enum variants, etc.
  // Function calls are handled in emit_call_expr, so here we handle
  // non-call access (e.g., module constants, enum variant tags).
  auto obj_sem = semantic_type(*node.object);
  if (obj_sem && obj_sem->kind == TypeKind::Module) {
    auto &mod = std::get<ModuleTypeInfo>(obj_sem->detail);
    for (auto &exp : mod.exports) {
      if (exp.name == field_name) {
        if (exp.type && exp.type->kind == TypeKind::Func) {
          // Function reference (not a call) — declare and return.
          auto *fn = declare_import(mod.name, field_name, exp.type);
          return fn;
        }
        // For enum variants from an imported module, look up the tag.
        if (exp.type && exp.type->kind == TypeKind::Enum) {
          auto &einfo = std::get<EnumTypeInfo>(exp.type->detail);
          // Register enum type and variants from the import if not
          // already known, so variant selectors can resolve them.
          if (!enum_types.count(einfo.name)) {
            enum_types[einfo.name] = true;
            int64_t next_index = 0;
            for (auto &v : einfo.variants) {
              if (v.index >= 0)
                next_index = v.index;
              enum_variants[einfo.name + "." + v.name] = next_index++;
            }
          }
          // Return a sentinel so chained selectors (.Write) can
          // look up the variant in the enum_variants map.
          return llvm::ConstantInt::get(i64_type, 0);
        }

        // Non-function, non-enum export: a module-level constant.
        // Declare (or find) an external global and load from it.
        {
          std::string gv_name = mangle(mod.name, field_name);
          auto *ll = llvm_type(exp.type);

          // For struct-typed constants, ensure the LLVM struct type exists.
          if (exp.type && exp.type->kind == TypeKind::Struct) {
            auto &sinfo = std::get<StructTypeInfo>(exp.type->detail);
            if (!struct_types.count(sinfo.name)) {
              // Register a minimal LLVM struct from the semantic info.
              std::vector<llvm::Type *> ftypes;
              std::vector<std::string> fnames;
              for (auto &f : sinfo.fields) {
                ftypes.push_back(llvm_type(f.type));
                fnames.push_back(f.name);
              }
              auto *st = llvm::StructType::create(context, ftypes,
                                                   "mc." + sinfo.name);
              struct_types[sinfo.name] = st;
              struct_fields[sinfo.name] = std::move(fnames);
            }
            ll = struct_types[sinfo.name];
          }

          auto *gv = module->getGlobalVariable(gv_name);
          if (!gv) {
            gv = new llvm::GlobalVariable(
                *module, ll, /*isConstant=*/true,
                llvm::GlobalValue::ExternalLinkage,
                /*Initializer=*/nullptr, gv_name);
          }
          return builder.CreateLoad(ll, gv, field_name);
        }
        break;
      }
    }
    return nullptr;
  }

  // Emit the object expression.  For a struct variable this will be a load
  // of a pointer (from the alloca).  But we need the alloca itself to GEP.
  // Check if the object is an identifier referencing a local struct.
  if (auto *ident = std::get_if<IdentifierNode>(&node.object->data)) {
    std::string obj_name(ident->name);
    auto local_it = locals.find(obj_name);
    if (local_it != locals.end()) {
      auto *alloca = local_it->second;
      // Check if this alloca holds a struct type.
      auto sem = semantic_type(*node.object);
      if (sem && sem->kind == TypeKind::Struct) {
        auto &info = std::get<StructTypeInfo>(sem->detail);
        auto st_it = struct_types.find(info.name);
        if (st_it != struct_types.end()) {
          // The alloca might be a ptr to struct (if stored from a literal)
          // or the struct type itself.
          auto *alloca_type = alloca->getAllocatedType();
          if (alloca_type == st_it->second) {
            // Direct struct alloca — GEP into it.
            auto [gep, ftype] = struct_field_gep(alloca, sem, field_name);
            if (gep)
              return builder.CreateLoad(ftype, gep, field_name);
          } else if (alloca_type->isPointerTy()) {
            // Pointer to struct — load the pointer, then GEP.
            auto *ptr = builder.CreateLoad(alloca_type, alloca, obj_name);
            auto [gep, ftype] = struct_field_gep(ptr, sem, field_name);
            if (gep)
              return builder.CreateLoad(ftype, gep, field_name);
          }
        }
      }
    }
  }

  // Fallback: emit the object as a value (pointer), then GEP.
  auto *obj = emit_expr(*node.object);
  if (!obj)
    return nullptr;

  auto sem = semantic_type(*node.object);
  if (sem && sem->kind == TypeKind::Struct) {
    auto [gep, ftype] = struct_field_gep(obj, sem, field_name);
    if (gep)
      return builder.CreateLoad(ftype, gep, field_name);
  }

  // Enum variant access: EnumName.Variant → integer constant.
  if (sem && sem->kind == TypeKind::Enum) {
    auto &info = std::get<EnumTypeInfo>(sem->detail);
    std::string key = info.name + "." + field_name;
    auto ev_it = enum_variants.find(key);
    if (ev_it != enum_variants.end())
      return llvm::ConstantInt::get(i64_type, ev_it->second);
  }

  // Array/other built-in methods accessed via selector (handled in call).
  return nullptr;
}

// ===========================================================================
// Call expressions
// ===========================================================================

llvm::Value *CodeGen::emit_call_expr(const CallExprNode &node,
                                     const Node &parent) {
  // Check for method calls on objects (selector calls like arr.Size()).
  if (auto *sel = std::get_if<SelectorNode>(&node.callee->data)) {
    std::string method(sel->field.name);

    auto obj_sem = semantic_type(*sel->object);

    // ── Module function call: mod.Func(args) ────────────────────────
    if (obj_sem && obj_sem->kind == TypeKind::Module) {
      auto &mod = std::get<ModuleTypeInfo>(obj_sem->detail);
      // Find the export.
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

      auto &fn_info = std::get<FuncTypeInfo>(func_type->detail);
      std::vector<llvm::Value *> args;

      if (fn_info.is_variadic && !fn_info.params.empty()) {
        // Pack variadic arguments: non-variadic params are emitted
        // normally, then the remaining args are packed into an mc_array.
        size_t fixed_count = fn_info.params.size() - 1;
        for (size_t i = 0; i < fixed_count && i < node.args.size(); ++i) {
          auto *val = emit_expr(*node.args[i]);
          if (val) args.push_back(val);
        }
        // Build the variadic array.
        size_t var_count = node.args.size() > fixed_count
                               ? node.args.size() - fixed_count : 0;
        auto *arr = builder.CreateCall(
            module->getFunction("saga_array_new"),
            {llvm::ConstantInt::get(i64_type, 8),
             llvm::ConstantInt::get(i64_type, var_count)}, "varargs");
        auto *func = builder.GetInsertBlock()->getParent();
        for (size_t i = 0; i < var_count; ++i) {
          auto *val = emit_expr(*node.args[fixed_count + i]);
          if (!val) continue;
          auto *tmp = create_entry_alloca(func, "va.tmp", val->getType());
          builder.CreateStore(val, tmp);
          builder.CreateCall(module->getFunction("saga_array_push"),
                             {arr, tmp});
        }
        args.push_back(arr);
      } else {
        for (size_t i = 0; i < node.args.size(); ++i) {
          auto *val = emit_expr(*node.args[i]);
          if (!val) continue;
          // If the parameter type is a union but the argument is
          // concrete, wrap the value into the union struct.
          if (i < fn_info.params.size() &&
              fn_info.params[i]->kind == TypeKind::Union) {
            auto arg_sem = semantic_type(*node.args[i]);
            if (arg_sem && arg_sem->kind != TypeKind::Union) {
              auto *wrapped = emit_union_wrap(val, arg_sem, fn_info.params[i]);
              if (wrapped) {
                auto *union_st = get_union_llvm_type(fn_info.params[i]);
                val = builder.CreateLoad(union_st, wrapped, "arg.union");
              }
            }
          }
          args.push_back(val);
        }
      }

      if (callee->getReturnType()->isVoidTy()) {
        builder.CreateCall(callee, args);
        return nullptr;
      }
      return builder.CreateCall(callee, args, "pkg.call");
    }

    // ── Struct-field function call: r.handler(args) where `handler` is a
    //    field of function type.  Load the fn pointer from the field, then
    //    call it directly.
    if (obj_sem && obj_sem->kind == TypeKind::Struct) {
      auto &sinfo = std::get<StructTypeInfo>(obj_sem->detail);
      for (auto &fld : sinfo.fields) {
        if (fld.name != method) continue;
        if (!fld.type || fld.type->kind != TypeKind::Func) break;

        // Resolve the struct ptr — prefer the alloca for a local identifier
        // so we can GEP without extracting from a loaded value.
        auto *ptr_type = llvm::PointerType::getUnqual(context);
        llvm::Value *struct_ptr = nullptr;
        if (auto *id = std::get_if<IdentifierNode>(&sel->object->data)) {
          auto lit = locals.find(std::string(id->name));
          if (lit != locals.end()) {
            auto *alloca = lit->second;
            auto st_it = struct_types.find(sinfo.name);
            if (st_it != struct_types.end() &&
                alloca->getAllocatedType() == st_it->second) {
              struct_ptr = alloca;
            } else if (alloca->getAllocatedType()->isPointerTy()) {
              struct_ptr = builder.CreateLoad(ptr_type, alloca, "obj.load");
            }
          }
        }
        if (!struct_ptr)
          struct_ptr = emit_expr(*sel->object);
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
        llvm::Type *ret_ll = fi.returns.empty()
                                 ? void_ll_type : llvm_type(fi.returns[0]);
        auto *fn_type = llvm::FunctionType::get(ret_ll, param_types, false);
        if (ret_ll->isVoidTy()) {
          builder.CreateCall(fn_type, fn_ptr, args);
          return nullptr;
        }
        return builder.CreateCall(fn_type, fn_ptr, args, "field.call");
      }
    }

    auto *obj = emit_expr(*sel->object);
    if (!obj)
      return nullptr;

    // Stdlib-defined receiver methods on generic intrinsic types (Array, Map).
    // These are compiled from stdlib source (e.g. std/array/array.sg) and use
    // opaque ptr for TypeParam parameters.  At the call site we box/unbox
    // concrete scalar values to match the generic ABI.
    if (obj_sem) {
      auto km_it = analyzer.kind_methods_.find(obj_sem->kind);
      if (km_it != analyzer.kind_methods_.end()) {
        for (auto &m : km_it->second) {
          if (m.name != method)
            continue;

          // Determine the type name for the mangled link name.
          const char *km_type_name = nullptr;
          switch (obj_sem->kind) {
          case TypeKind::Array: km_type_name = "Array"; break;
          case TypeKind::Map:   km_type_name = "Map"; break;
          default: break;
          }
          if (!km_type_name)
            break;

          // Look up or forward-declare the stdlib function.
          std::string same_pkg_link =
              mangle(std::string(km_type_name) + "__" + method);
          auto *callee = module->getFunction(same_pkg_link);

          if (!callee && m.signature &&
              m.signature->kind == TypeKind::Func) {
            // Cross-package call: forward-declare the external function.
            std::string stdlib_pkg = std::string(km_type_name);
            std::transform(stdlib_pkg.begin(), stdlib_pkg.end(),
                           stdlib_pkg.begin(), ::tolower);
            std::string cross_link = mangle(stdlib_pkg,
                std::string(km_type_name) + "__" + method);

            callee = module->getFunction(cross_link);
            if (!callee) {
              auto &fi = std::get<FuncTypeInfo>(m.signature->detail);
              auto *self_ll = llvm_type(obj_sem);
              std::vector<llvm::Type *> param_ll;
              param_ll.push_back(self_ll); // self
              for (auto &p : fi.params)
                param_ll.push_back(llvm_type(p));
              llvm::Type *ret_ll = fi.returns.empty()
                                       ? void_ll_type
                                       : llvm_type(fi.returns[0]);
              auto *ft = llvm::FunctionType::get(ret_ll, param_ll, false);
              callee = llvm::Function::Create(
                  ft, llvm::Function::ExternalLinkage, cross_link,
                  module.get());
            }
          }

          if (callee) {
            auto *parent_fn = builder.GetInsertBlock()->getParent();
            auto *callee_ft = callee->getFunctionType();

            std::vector<llvm::Value *> args;
            args.push_back(obj); // self

            // Check which params are TypeParam (generic) in the signature.
            std::vector<bool> param_is_generic;
            if (m.signature) {
              auto &fi = std::get<FuncTypeInfo>(m.signature->detail);
              for (auto &pt : fi.params)
                param_is_generic.push_back(
                    pt && pt->kind == TypeKind::TypeParam);
            }

            for (size_t ai = 0; ai < node.args.size(); ++ai) {
              auto *val = emit_expr(*node.args[ai]);
              if (!val) continue;
              // Box → ptr if callee param is a generic TypeParam.
              // All values (including pointers like strings) must be
              // boxed so the callee receives void* → actual value.
              size_t pi = ai + 1; // +1 for self
              bool is_generic = ai < param_is_generic.size() &&
                                param_is_generic[ai];
              if (is_generic && pi < callee_ft->getNumParams()) {
                auto *tmp = create_entry_alloca(parent_fn, "box.tmp",
                                                val->getType());
                builder.CreateStore(val, tmp);
                val = tmp;
              }
              args.push_back(val);
            }

            if (callee_ft->getReturnType()->isVoidTy()) {
              builder.CreateCall(callee, args);
              return nullptr;
            }

            llvm::Value *result = builder.CreateCall(callee, args, "kmcall");

            // Unbox: if callee returns ptr but the concrete return type
            // (after substituting type params) is a scalar, load it.
            if (result->getType()->isPointerTy() && m.signature) {
              auto &fi = std::get<FuncTypeInfo>(m.signature->detail);
              if (!fi.returns.empty() &&
                  fi.returns[0]->kind == TypeKind::TypeParam) {
                // Resolve the concrete type from the receiver.
                TypePtr concrete_ret;
                if (obj_sem->kind == TypeKind::Array) {
                  auto &arr_info = std::get<ArrayTypeInfo>(obj_sem->detail);
                  concrete_ret = arr_info.element;
                } else if (obj_sem->kind == TypeKind::Map) {
                  auto &map_info = std::get<MapTypeInfo>(obj_sem->detail);
                  auto &tp = std::get<TypeParamInfo>(fi.returns[0]->detail);
                  if (tp.param.id == 9991)
                    concrete_ret = map_info.key;
                  else
                    concrete_ret = map_info.value;
                }
                if (concrete_ret) {
                  auto *concrete_ll = llvm_type(concrete_ret);
                  if (!concrete_ll->isPointerTy() &&
                      !concrete_ll->isVoidTy()) {
                    result = builder.CreateLoad(concrete_ll, result, "unbox");
                  }
                }
              }
            }

            return result;
          }
          break;
        }
      }
    }


    // Enum .Int() — enums are already i64 index values in LLVM IR.
    if (method == "Int" && obj_sem && obj_sem->kind == TypeKind::Enum) {
      return obj;
    }

    // Enum .String() — convert the enum's integer index to a string.
    if (method == "String" && obj_sem && obj_sem->kind == TypeKind::Enum) {
      return builder.CreateCall(
          module->getFunction("saga_int_to_string"), {obj}, "str");
    }

    // ── Task method calls ─────────────────────────────────────────────
    // Task is a semantic struct wrapping mc_actor*.  obj is the actor ptr.
    if (obj_sem && obj_sem->kind == TypeKind::Struct) {
      auto &sinfo = std::get<StructTypeInfo>(obj_sem->detail);
      if (sinfo.name == "Task") {
        if (method == "Alive?") {
          auto *fn = module->getFunction("saga_task_alive");
          auto *result = builder.CreateCall(fn, {obj}, "alive");
          return builder.CreateICmpNE(result,
              llvm::ConstantInt::get(i64_type, 0), "alive.bool");
        }
        if (method == "Cancel") {
          builder.CreateCall(module->getFunction("saga_task_cancel"), {obj});
          return nullptr;
        }
        if (method == "Term") {
          builder.CreateCall(module->getFunction("saga_task_term"), {obj});
          return nullptr;
        }
        if (method == "Wait") {
          // task.Wait() lowers to: call saga_task_wait; branch on status.
          //   COMPLETED → wrap the loaded T value as the T branch.
          //   anything else → call saga_error_from_trap, wrap the returned
          //     Error interface fat pointer as the Error branch.
          // Returns a pointer to the mc.union.T|Error struct.
          auto *func = builder.GetInsertBlock()->getParent();
          auto *ptr_type = llvm::PointerType::getUnqual(context);

          // Derive the union type (T | Error) for this call.  The
          // concrete T is the Task struct's first type argument.
          TypePtr t_sem;
          if (!sinfo.type_args.empty())
            t_sem = sinfo.type_args[0];
          if (!t_sem) {
            // No concrete T — semantic analysis should have bound it.
            return nullptr;
          }
          auto error_iface_sem_pre = analyzer.builtins.error_iface;
          auto union_sem = make_union_type({t_sem, error_iface_sem_pre});
          auto *union_st = get_union_llvm_type(union_sem);

          auto *status_alloca = create_entry_alloca(func, "wait.status",
                                                     i64_type);
          builder.CreateStore(llvm::ConstantInt::get(i64_type, 0),
                              status_alloca);
          auto *result_ptr = builder.CreateCall(
              module->getFunction("saga_task_wait"),
              {obj, status_alloca}, "wait.result");
          auto *status = builder.CreateLoad(i64_type, status_alloca,
                                             "wait.status.val");
          auto *is_ok = builder.CreateICmpEQ(status,
              llvm::ConstantInt::get(i64_type, /*MC_ACTOR_COMPLETED=*/2),
              "wait.ok");

          auto *bb_ok = llvm::BasicBlock::Create(context, "wait.ok.bb", func);
          auto *bb_err = llvm::BasicBlock::Create(context, "wait.err.bb");
          auto *bb_merge = llvm::BasicBlock::Create(context, "wait.merge");
          builder.CreateCondBr(is_ok, bb_ok, bb_err);

          // ── Success path: load T from result_ptr, wrap as T branch. ───
          builder.SetInsertPoint(bb_ok);
          llvm::Value *wrapped_ok = nullptr;
          if (t_sem) {
            auto *t_ll = llvm_type(t_sem);
            llvm::Value *t_val = nullptr;
            if (t_ll->isVoidTy()) {
              // Void T — no payload to load; wrap a zero.
              t_val = llvm::ConstantInt::get(
                  llvm::Type::getInt8Ty(context), 0);
              wrapped_ok = emit_union_wrap(t_val, t_sem, union_sem);
            } else {
              t_val = builder.CreateLoad(t_ll, result_ptr, "wait.t.val");
              wrapped_ok = emit_union_wrap(t_val, t_sem, union_sem);
            }
          }
          if (!wrapped_ok)
            wrapped_ok = llvm::ConstantPointerNull::get(ptr_type);
          auto *bb_ok_end = builder.GetInsertBlock();
          builder.CreateBr(bb_merge);

          // ── Error path: build TrapError fat pointer, wrap as Error. ──
          func->insert(func->end(), bb_err);
          builder.SetInsertPoint(bb_err);
          auto *err_fat = builder.CreateCall(
              module->getFunction("saga_error_from_trap"),
              {obj}, "wait.err.fat");
          // error_iface has kind Interface; emit_union_wrap finds its tag
          // via the interface-satisfaction path in union_tag_for_type.
          auto error_iface_sem = analyzer.builtins.error_iface;
          auto *wrapped_err = emit_union_wrap(err_fat, error_iface_sem,
                                               union_sem);
          if (!wrapped_err)
            wrapped_err = llvm::ConstantPointerNull::get(ptr_type);
          auto *bb_err_end = builder.GetInsertBlock();
          builder.CreateBr(bb_merge);

          // ── Merge: PHI the two union pointers. ──────────────────────
          func->insert(func->end(), bb_merge);
          builder.SetInsertPoint(bb_merge);
          auto *phi = builder.CreatePHI(ptr_type, 2, "wait.union");
          phi->addIncoming(wrapped_ok, bb_ok_end);
          phi->addIncoming(wrapped_err, bb_err_end);
          (void)union_st; // silence unused if opaque
          return phi;
        }
        // Fall through to generic struct method handling if not matched.
      }

      // ── Context method calls (inside spawn body) ─────────────────
      if (sinfo.name == "Context") {
        if (method == "Cancelled?") {
          auto *fn = module->getFunction("saga_context_cancelled");
          auto *result = builder.CreateCall(fn, {obj}, "cancelled");
          return builder.CreateICmpNE(result,
              llvm::ConstantInt::get(i64_type, 0), "cancelled.bool");
        }
        if (method == "Send" && !node.args.empty()) {
          auto *val = emit_expr(*node.args[0]);
          if (!val) return nullptr;
          auto *func = builder.GetInsertBlock()->getParent();
          // If val is already a pointer to the payload (struct literal,
          // union alloca), pass it directly — the runtime memcpys
          // elem_size bytes from the given address.  Otherwise (scalar
          // value like an Int), spill to a temp alloca first.
          llvm::Value *data_ptr = val;
          auto arg_sem = semantic_type(*node.args[0]);
          bool val_is_ptr_to_payload =
              val->getType()->isPointerTy() && arg_sem &&
              (arg_sem->kind == TypeKind::Struct ||
               arg_sem->kind == TypeKind::Union);
          if (!val_is_ptr_to_payload) {
            auto *tmp = create_entry_alloca(func, "send.tmp", val->getType());
            builder.CreateStore(val, tmp);
            data_ptr = tmp;
          }
          builder.CreateCall(module->getFunction("saga_context_send"),
                             {obj, data_ptr});
          return nullptr;
        }
        if (method == "Exit") {
          auto *func = builder.GetInsertBlock()->getParent();
          if (!node.args.empty()) {
            auto *val = emit_expr(*node.args[0]);
            if (val) {
              auto *tmp = create_entry_alloca(func, "exit.tmp",
                                               val->getType());
              builder.CreateStore(val, tmp);
              auto &dl = module->getDataLayout();
              uint64_t sz = dl.getTypeAllocSize(val->getType());
              builder.CreateCall(
                  module->getFunction("saga_context_exit"),
                  {obj, tmp,
                   llvm::ConstantInt::get(i64_type, sz)});
            }
          } else {
            auto *null_ptr = llvm::ConstantPointerNull::get(
                llvm::PointerType::getUnqual(context));
            builder.CreateCall(
                module->getFunction("saga_context_exit"),
                {obj, null_ptr, llvm::ConstantInt::get(i64_type, 0)});
          }
          return nullptr;
        }
        // Fall through to generic struct method handling if not matched.
      }
    }

    // Stdlib-defined receiver method on an intrinsic type.
    // Check the analyzer's type_methods_ side table; if the method was
    // registered there, call the mangled function (pkg__Type__Method).
    if (obj_sem) {
      // Look up by raw pointer first, then fall back to the canonical
      // builtin type pointer (node_types may hold a different shared_ptr
      // than the one used to key type_methods_ during prelude loading).
      const Type *raw = obj_sem.get();
      auto tm_it = analyzer.type_methods_.find(raw);
      if (tm_it == analyzer.type_methods_.end()) {
        // Resolve to the canonical builtin pointer for this type.
        const Type *canonical = nullptr;
        switch (obj_sem->kind) {
        case TypeKind::Int: {
          auto &ii = std::get<IntType>(obj_sem->detail);
          if (ii.bits == 0) {
            canonical = (ii.is_signed ? analyzer.builtins.int_type
                                      : analyzer.builtins.int_type).get();
          } else if (ii.is_signed) {
            switch (ii.bits) {
            case 8:  canonical = analyzer.builtins.int8_type.get(); break;
            case 16: canonical = analyzer.builtins.int16_type.get(); break;
            case 32: canonical = analyzer.builtins.int32_type.get(); break;
            case 64: canonical = analyzer.builtins.int64_type.get(); break;
            }
          } else {
            switch (ii.bits) {
            case 8:  canonical = analyzer.builtins.uint8_type.get(); break;
            case 16: canonical = analyzer.builtins.uint16_type.get(); break;
            case 32: canonical = analyzer.builtins.uint32_type.get(); break;
            case 64: canonical = analyzer.builtins.uint64_type.get(); break;
            }
          }
          break;
        }
        case TypeKind::Float:  canonical = analyzer.builtins.float_type.get(); break;
        case TypeKind::Bool:   canonical = analyzer.builtins.bool_type.get(); break;
        case TypeKind::String: canonical = analyzer.builtins.string_type.get(); break;
        default: break;
        }
        if (canonical && canonical != raw)
          tm_it = analyzer.type_methods_.find(canonical);
      }
      if (tm_it != analyzer.type_methods_.end()) {
        for (auto &m : tm_it->second) {
          if (m.name != method)
            continue;
          // Determine the type name and origin package.
          // Sized int types (Int8..Uint64) live in the "int" package;
          // the type name must match the receiver (e.g. "Int64").
          std::string tn = type_to_string(obj_sem);
          const char *stdlib_pkg_name = nullptr;
          switch (obj_sem->kind) {
          case TypeKind::Int:    stdlib_pkg_name = "int"; break;
          case TypeKind::Float:  stdlib_pkg_name = "float"; break;
          case TypeKind::Bool:   stdlib_pkg_name = "bool"; break;
          case TypeKind::String: stdlib_pkg_name = "string"; break;
          default: break;
          }
          if (!stdlib_pkg_name)
            break;

          // The stdlib defines methods as `pkg__TypeName__Method` where
          // pkg is the package name (e.g. "int__Int64__String").
          // First check if the method is defined in the current module
          // (happens when a stdlib package compiles its own methods).
          std::string same_pkg_link =
              mangle(tn + "__" + method);
          auto *callee = module->getFunction(same_pkg_link);

          if (!callee && m.signature &&
              m.signature->kind == TypeKind::Func) {
            // Cross-package call: the method lives in the stdlib type package.
            std::string cross_link = mangle(stdlib_pkg_name,
                tn + "__" + method);

            callee = module->getFunction(cross_link);
            if (!callee) {
              // Forward-declare the external function so the linker resolves it.
              auto &fi = std::get<FuncTypeInfo>(m.signature->detail);
              auto *self_ll = llvm_type(obj_sem);
              std::vector<llvm::Type *> param_ll;
              param_ll.push_back(self_ll); // self
              for (auto &p : fi.params)
                param_ll.push_back(llvm_type(p));
              llvm::Type *ret_ll = fi.returns.empty()
                                       ? void_ll_type
                                       : llvm_type(fi.returns[0]);
              auto *ft = llvm::FunctionType::get(ret_ll, param_ll, false);
              callee = llvm::Function::Create(
                  ft, llvm::Function::ExternalLinkage, cross_link,
                  module.get());
            }
          }

          if (callee) {
            std::vector<llvm::Value *> args;
            args.push_back(obj); // self (value, not pointer)
            for (auto &arg_node : node.args) {
              auto *val = emit_expr(*arg_node);
              if (val)
                args.push_back(val);
            }
            if (callee->getReturnType()->isVoidTy()) {
              builder.CreateCall(callee, args);
              return nullptr;
            }
            return builder.CreateCall(callee, args, "mcall");
          }
          break;
        }
      }
    }

    // Struct method call: obj.Method(args) → StructName.Method(obj, args).
    if (obj_sem && obj_sem->kind == TypeKind::Struct) {
      auto &info = std::get<StructTypeInfo>(obj_sem->detail);

      // Determine the package that defines this struct.
      // If the struct exists in our local struct_types, use package_name.
      // Otherwise, check if the object originates from a module selector
      // and use that module's name.
      // Determine the package that defines this struct.
      // Walk the selector chain to find if the object originates from an
      // imported module. For chained selectors like os.Stdout.Write,
      // the inner selector's object is the module.
      std::string origin_pkg = package_name;
      {
        const Node *walk = sel->object.get();
        while (walk) {
          if (auto *inner_sel = std::get_if<SelectorNode>(&walk->data)) {
            auto inner_sem = semantic_type(*inner_sel->object);
            if (inner_sem && inner_sem->kind == TypeKind::Module) {
              origin_pkg = std::get<ModuleTypeInfo>(inner_sem->detail).name;
              break;
            }
            walk = inner_sel->object.get();
          } else if (auto *id = std::get_if<IdentifierNode>(&walk->data)) {
            auto id_sem = semantic_type(*walk);
            if (id_sem && id_sem->kind == TypeKind::Module) {
              origin_pkg = std::get<ModuleTypeInfo>(id_sem->detail).name;
            }
            break;
          } else {
            break;
          }
        }
      }

      std::string link_name = mangle(origin_pkg, info.name + "__" + method);
      auto *callee = module->getFunction(link_name);

      // If the method isn't in this module (cross-package struct), declare it.
      if (!callee) {
        for (auto &m : info.methods) {
          if (m.name == method && m.signature &&
              m.signature->kind == TypeKind::Func) {
            auto &finfo = std::get<FuncTypeInfo>(m.signature->detail);
            auto *ptr_type = llvm::PointerType::getUnqual(context);
            std::vector<llvm::Type *> param_ll;
            param_ll.push_back(ptr_type); // self
            for (auto &p : finfo.params)
              param_ll.push_back(llvm_type(p));
            llvm::Type *ret_ll = finfo.returns.empty()
                                     ? void_ll_type
                                     : llvm_type(finfo.returns[0]);
            auto *ft = llvm::FunctionType::get(ret_ll, param_ll, false);
            callee = llvm::Function::Create(
                ft, llvm::Function::ExternalLinkage, link_name, module.get());
            break;
          }
        }
      }

      if (callee) {
        // Build argument list: self + explicit args.
        // For struct methods, self is a pointer to the struct.
        // obj might be a loaded pointer or a direct alloca — we need
        // the alloca (pointer) for self.
        llvm::Value *self_ptr = obj;

        // If the object is an identifier referencing a local, use the alloca.
        if (auto *id = std::get_if<IdentifierNode>(&sel->object->data)) {
          auto local_it = locals.find(std::string(id->name));
          if (local_it != locals.end()) {
            auto *alloca = local_it->second;
            auto *alloca_type = alloca->getAllocatedType();
            auto st_it = struct_types.find(info.name);
            if (st_it != struct_types.end() && alloca_type == st_it->second) {
              self_ptr = alloca; // Direct struct alloca.
            } else if (alloca_type->isPointerTy()) {
              self_ptr = builder.CreateLoad(alloca_type, alloca, "self.ptr");
            }
          }
        }

        // If self_ptr is a struct value (not a pointer/alloca), we need
        // to spill it to a temporary alloca so the method gets a ptr.
        auto st_it2 = struct_types.find(info.name);
        if (st_it2 != struct_types.end() &&
            self_ptr->getType() == st_it2->second) {
          auto *func = builder.GetInsertBlock()->getParent();
          auto *tmp = create_entry_alloca(func, "self.tmp", st_it2->second);
          builder.CreateStore(self_ptr, tmp);
          self_ptr = tmp;
        }

        std::vector<llvm::Value *> args;
        args.push_back(self_ptr);
        for (auto &arg_node : node.args) {
          auto *val = emit_expr(*arg_node);
          if (val)
            args.push_back(val);
        }
        if (callee->getReturnType()->isVoidTy()) {
          builder.CreateCall(callee, args);
          return nullptr;
        }
        return builder.CreateCall(callee, args, "mcall");
      }
    }

    // Interface dynamic dispatch: obj.Method(args) via vtable.
    if (obj_sem && obj_sem->kind == TypeKind::Interface) {
      auto &iface_info = std::get<InterfaceTypeInfo>(obj_sem->detail);
      std::string iface_name = iface_info.name;

      auto mn_it = iface_method_names.find(iface_name);
      auto vt_it = iface_vtable_types.find(iface_name);
      if (mn_it != iface_method_names.end() &&
          vt_it != iface_vtable_types.end()) {
        auto &methods = mn_it->second;
        auto *vtable_st = vt_it->second;

        // Find the method index.
        int method_idx = -1;
        for (size_t i = 0; i < methods.size(); ++i) {
          if (methods[i] == method) {
            method_idx = static_cast<int>(i);
            break;
          }
        }
        if (method_idx >= 0) {
          // obj is a ptr to mc_iface { ptr data, ptr vtable }.
          // Load data and vtable pointers.
          auto *ptr_type = llvm::PointerType::getUnqual(context);

          // If obj is an identifier, get the alloca for the fat pointer.
          llvm::Value *fat_ptr = obj;
          if (auto *id = std::get_if<IdentifierNode>(&sel->object->data)) {
            auto local_it = locals.find(std::string(id->name));
            if (local_it != locals.end()) {
              auto *alloca = local_it->second;
              if (alloca->getAllocatedType() == iface_fat_ptr_type) {
                fat_ptr = alloca;
              } else if (alloca->getAllocatedType()->isPointerTy()) {
                fat_ptr = builder.CreateLoad(ptr_type, alloca, "fat.load");
              }
            }
          }

          auto *data_gep = builder.CreateStructGEP(
              iface_fat_ptr_type, fat_ptr, 0, "iface.data.ptr");
          auto *data_ptr = builder.CreateLoad(ptr_type, data_gep, "data");

          auto *vtable_gep = builder.CreateStructGEP(
              iface_fat_ptr_type, fat_ptr, 1, "iface.vtable.ptr");
          auto *vtable_ptr = builder.CreateLoad(ptr_type, vtable_gep, "vtable");

          // Load the function pointer from the vtable.
          auto *fn_gep = builder.CreateStructGEP(
              vtable_st, vtable_ptr, method_idx, "vfn.ptr");
          auto *fn_ptr = builder.CreateLoad(ptr_type, fn_gep, "vfn");

          // Build the function type for the indirect call.
          // First param is the data pointer (self), then explicit args.
          std::vector<llvm::Type *> param_ll_types;
          param_ll_types.push_back(ptr_type); // self

          std::vector<llvm::Value *> args;
          args.push_back(data_ptr);

          for (auto &arg_node : node.args) {
            auto *val = emit_expr(*arg_node);
            if (val) {
              args.push_back(val);
              param_ll_types.push_back(val->getType());
            }
          }

          // Determine return type from the interface method signature.
          llvm::Type *ret_ll = void_ll_type;
          for (auto &im : iface_info.methods) {
            if (im.name == method && im.signature) {
              auto &fi = std::get<FuncTypeInfo>(im.signature->detail);
              if (!fi.returns.empty())
                ret_ll = llvm_type(fi.returns[0]);
              break;
            }
          }

          auto *fn_type = llvm::FunctionType::get(ret_ll, param_ll_types, false);

          if (ret_ll->isVoidTy()) {
            builder.CreateCall(fn_type, fn_ptr, args);
            return nullptr;
          }
          return builder.CreateCall(fn_type, fn_ptr, args, "iface.call");
        }
      }
    }

    return nullptr;
  }

  // Direct function call.
  auto *ident = std::get_if<IdentifierNode>(&node.callee->data);
  if (!ident)
    return nullptr;

  std::string name(ident->name);

  // ── Concurrency intrinsics ──────────────────────────────────────────
  // These need special handling because they inject the current_actor
  // pointer or emit inline LLVM instructions.

  // ── New stdlib intrinsics ────────────────────────────────────────────

  if (name == "intrinsic_sitofp") {
    // intrinsic_sitofp(value: Int) -> Float (f64)
    auto *val = emit_expr(*node.args[0]);
    if (!val) return nullptr;
    return builder.CreateSIToFP(val, f64_type, "sitofp");
  }

  if (name == "intrinsic_sitofp32") {
    // intrinsic_sitofp32(value: Int) -> Float32 (f32)
    auto *val = emit_expr(*node.args[0]);
    if (!val) return nullptr;
    return builder.CreateSIToFP(val, llvm::Type::getFloatTy(context), "sitofp32");
  }

  if (name == "intrinsic_fptrunc") {
    // intrinsic_fptrunc(value: Float) -> Float32 (f64 → f32)
    auto *val = emit_expr(*node.args[0]);
    if (!val) return nullptr;
    return builder.CreateFPTrunc(val, llvm::Type::getFloatTy(context), "fptrunc");
  }

  if (name == "intrinsic_fpext") {
    // intrinsic_fpext(value: Float) -> Float64 (f32 → f64)
    // On 64-bit targets this is a no-op since Float is already f64.
    auto *val = emit_expr(*node.args[0]);
    if (!val) return nullptr;
    auto *dst = llvm::Type::getDoubleTy(context);
    if (val->getType() == dst) return val; // already f64 — identity
    return builder.CreateFPExt(val, dst, "fpext");
  }

  if (name == "intrinsic_fptosi") {
    // intrinsic_fptosi(value: Float) -> Int
    auto *val = emit_expr(*node.args[0]);
    if (!val) return nullptr;
    return builder.CreateFPToSI(val, i64_type, "fptosi");
  }

  if (name == "intrinsic_zext") {
    // intrinsic_zext(value: Int, bits: Int) -> Int
    // bits must be an integer literal.
    auto *val = emit_expr(*node.args[0]);
    if (!val) return nullptr;
    auto *bits_node = std::get_if<IntegerLiteralNode>(&node.args[1]->data);
    if (!bits_node) return nullptr;
    int64_t bits = parse_int_literal(bits_node->literal);
    auto *dst_type = llvm::IntegerType::get(context, static_cast<unsigned>(bits));
    unsigned src_bits = val->getType()->getIntegerBitWidth();
    if (bits < static_cast<int64_t>(src_bits))
      return builder.CreateTrunc(val, dst_type, "ztrunc");
    if (bits > static_cast<int64_t>(src_bits))
      return builder.CreateZExt(val, dst_type, "zext");
    return val;
  }

  if (name == "intrinsic_sext") {
    // intrinsic_sext(value: Int, bits: Int) -> Int
    auto *val = emit_expr(*node.args[0]);
    if (!val) return nullptr;
    auto *bits_node = std::get_if<IntegerLiteralNode>(&node.args[1]->data);
    if (!bits_node) return nullptr;
    int64_t bits = parse_int_literal(bits_node->literal);
    auto *dst_type = llvm::IntegerType::get(context, static_cast<unsigned>(bits));
    unsigned src_bits = val->getType()->getIntegerBitWidth();
    if (bits < static_cast<int64_t>(src_bits))
      return builder.CreateTrunc(val, dst_type, "strunc");
    if (bits > static_cast<int64_t>(src_bits))
      return builder.CreateSExt(val, dst_type, "sext");
    return val;
  }

  if (name == "intrinsic_field") {
    // intrinsic_field(value: Any, index: Int) -> Any
    // Second arg must be an integer literal (field index).
    auto *val = emit_expr(*node.args[0]);
    if (!val) return nullptr;
    auto *idx_node = std::get_if<IntegerLiteralNode>(&node.args[1]->data);
    if (!idx_node) return nullptr;
    unsigned field_idx = static_cast<unsigned>(parse_int_literal(idx_node->literal));

    // The value must be a pointer; determine the backing struct type.
    // For String (mc_string*), use the string_type struct.
    auto arg_sem = semantic_type(*node.args[0]);
    llvm::StructType *backing_st = nullptr;
    if (arg_sem && arg_sem->kind == TypeKind::String) {
      backing_st = string_type;
    } else {
      // Try to find a struct type matching the value's pointee.
      // Walk struct_types looking for a match.
      for (auto &[sname, st] : struct_types) {
        (void)sname;
        if (val->getType() == llvm::PointerType::getUnqual(context)) {
          backing_st = st;
          break;
        }
      }
    }
    if (!backing_st) return nullptr;

    auto *gep = builder.CreateStructGEP(backing_st, val, field_idx, "field.ptr");
    // Return type: load the field. We return i64 for most fields.
    auto *field_type = backing_st->getElementType(field_idx);
    return builder.CreateLoad(field_type, gep, "field.val");
  }

  if (name == "intrinsic_runtime") {
    // intrinsic_runtime("func_name", args...) -> result
    // First arg must be a plain string literal naming a declared C runtime
    // function.  Extract the name from the first fragment of the string.
    if (node.args.empty()) return nullptr;
    auto *name_node = std::get_if<StringLiteralNode>(&node.args[0]->data);
    if (!name_node || name_node->fragments.empty()) return nullptr;
    auto *frag = std::get_if<StringFragmentNode>(&name_node->fragments[0]->data);
    if (!frag) return nullptr;
    // Strip surrounding quotes from the raw fragment text.
    std::string_view raw = frag->text;
    if (raw.size() >= 2 && raw.front() == '"' && raw.back() == '"')
      raw = raw.substr(1, raw.size() - 2);
    std::string func_name(raw);
    auto *callee = module->getFunction(func_name);
    if (!callee) {
      // The function isn't in the module — this is a compile-time error.
      // Return null to signal failure; semantic analysis gates this.
      return nullptr;
    }

    // Build argument list: skip args[0] (the name literal), emit the rest.
    std::vector<llvm::Value *> args;
    auto *fn_type = callee->getFunctionType();
    for (size_t i = 1; i < node.args.size(); ++i) {
      auto *val = emit_expr(*node.args[i]);
      if (!val) continue;

      // Auto-promote scalar to pointer if the C param expects a pointer.
      size_t param_idx = i - 1;
      if (param_idx < fn_type->getNumParams()) {
        auto *expected = fn_type->getParamType(param_idx);
        if (expected->isPointerTy() && !val->getType()->isPointerTy()) {
          auto *func = builder.GetInsertBlock()->getParent();
          auto *tmp = create_entry_alloca(func, "rt.tmp", val->getType());
          builder.CreateStore(val, tmp);
          val = tmp;
        }
      }
      args.push_back(val);
    }

    auto *ret_type = fn_type->getReturnType();
    if (ret_type->isVoidTy()) {
      builder.CreateCall(callee, args);
      return nullptr;
    }
    return builder.CreateCall(callee, args, "rt.call");
  }

  if (name == "intrinsic_runtime_try") {
    // intrinsic_runtime_try("func_name", args...) -> union
    // Calls a C function that writes its result to an auto-appended out-param
    // and returns i64 status (0 = success, non-zero = failure).
    // On success, wraps the out-param value as the success variant.
    // On failure, wraps Missing as the error/missing variant.
    if (node.args.empty()) return nullptr;
    auto *name_node = std::get_if<StringLiteralNode>(&node.args[0]->data);
    if (!name_node || name_node->fragments.empty()) return nullptr;
    auto *frag = std::get_if<StringFragmentNode>(&name_node->fragments[0]->data);
    if (!frag) return nullptr;
    std::string_view raw = frag->text;
    if (raw.size() >= 2 && raw.front() == '"' && raw.back() == '"')
      raw = raw.substr(1, raw.size() - 2);
    std::string func_name(raw);
    auto *callee = module->getFunction(func_name);
    if (!callee) return nullptr;

    // The enclosing function's semantic return type is the union we produce.
    auto union_sem = current_func_return_sem;
    if (!union_sem || union_sem->kind != TypeKind::Union) return nullptr;

    // Determine the success type (non-error/non-missing alternative).
    auto success_sem = strip_error_from_union(union_sem);
    if (!success_sem) return nullptr;
    auto *success_ll = llvm_type(success_sem);

    // Build argument list (same as intrinsic_runtime).
    std::vector<llvm::Value *> args;
    auto *fn_type = callee->getFunctionType();
    for (size_t i = 1; i < node.args.size(); ++i) {
      auto *val = emit_expr(*node.args[i]);
      if (!val) continue;
      size_t param_idx = i - 1;
      if (param_idx < fn_type->getNumParams()) {
        auto *expected = fn_type->getParamType(param_idx);
        if (expected->isPointerTy() && !val->getType()->isPointerTy()) {
          auto *parent_fn = builder.GetInsertBlock()->getParent();
          auto *tmp = create_entry_alloca(parent_fn, "rt.tmp", val->getType());
          builder.CreateStore(val, tmp);
          val = tmp;
        }
      }
      args.push_back(val);
    }

    // Create out-param alloca and append to args.
    auto *parent_fn = builder.GetInsertBlock()->getParent();
    auto *out_alloca = create_entry_alloca(parent_fn, "try.out", success_ll);
    builder.CreateStore(llvm::Constant::getNullValue(success_ll), out_alloca);
    args.push_back(out_alloca);

    // Call: returns i64 status.
    auto *status = builder.CreateCall(callee, args, "try.status");

    // Branch on status == 0.
    auto *is_ok = builder.CreateICmpEQ(
        status, llvm::ConstantInt::get(i64_type, 0), "try.ok");
    auto *bb_success = llvm::BasicBlock::Create(context, "try.success", parent_fn);
    auto *bb_fail = llvm::BasicBlock::Create(context, "try.fail", parent_fn);
    auto *bb_merge = llvm::BasicBlock::Create(context, "try.merge", parent_fn);
    builder.CreateCondBr(is_ok, bb_success, bb_fail);

    // Success path: load result, wrap in union.
    builder.SetInsertPoint(bb_success);
    auto *result_val = builder.CreateLoad(success_ll, out_alloca, "try.val");
    auto *wrapped_ok = emit_union_wrap(result_val, success_sem, union_sem);
    auto *bb_success_end = builder.GetInsertBlock(); // may have changed
    builder.CreateBr(bb_merge);

    // Failure path: wrap Missing in union.
    builder.SetInsertPoint(bb_fail);
    auto missing_sem = analyzer.builtins.missing_type;
    auto *missing_val = llvm::Constant::getNullValue(
        llvm::StructType::get(context)); // Missing is empty struct
    auto *wrapped_err = emit_union_wrap(missing_val, missing_sem, union_sem);
    auto *bb_fail_end = builder.GetInsertBlock();
    builder.CreateBr(bb_merge);

    // Merge with PHI.
    builder.SetInsertPoint(bb_merge);
    auto *union_st = get_union_llvm_type(union_sem);
    auto *phi = builder.CreatePHI(llvm::PointerType::getUnqual(context), 2, "try.result");
    phi->addIncoming(wrapped_ok, bb_success_end);
    phi->addIncoming(wrapped_err, bb_fail_end);
    return phi;
  }

  if (name == "intrinsic_yield") {
    // intrinsic_yield() → saga_actor_yield()
    // The runtime reads the current actor from a thread-local so this
    // call is safe from any depth inside a spawned execution context.
    // Outside an actor context the runtime itself no-ops on NULL.
    builder.CreateCall(module->getFunction("saga_actor_yield"), {});
    return llvm::Constant::getNullValue(
        llvm::PointerType::getUnqual(context));
  }

  if (name == "intrinsic_atomic_add") {
    // intrinsic_atomic_add(ptr, val) → atomicrmw add i64* %ptr, i64 %val
    // The first argument is treated as a pointer to an i64.
    // We need to get the *address* of the first argument, not its value.
    auto *val = emit_expr(*node.args[1]);
    // Get the address of the first argument (must be a variable).
    auto *ptr_ident = std::get_if<IdentifierNode>(&node.args[0]->data);
    llvm::Value *ptr = nullptr;
    if (ptr_ident) {
      auto it = locals.find(std::string(ptr_ident->name));
      if (it != locals.end())
        ptr = it->second; // alloca — this IS the pointer
    }
    if (!ptr) {
      // Fallback: emit the expression as a value (won't be atomic, but
      // won't crash).  Semantic analysis should catch misuse.
      return llvm::Constant::getNullValue(i64_type);
    }
    return builder.CreateAtomicRMW(llvm::AtomicRMWInst::Add, ptr, val,
                                   llvm::MaybeAlign(),
                                   llvm::AtomicOrdering::SequentiallyConsistent);
  }

  if (name == "intrinsic_trap") {
    // intrinsic_trap(reason) → saga_actor_trap(reason)
    // The runtime pulls the current actor from a thread-local so the
    // intrinsic works from any call depth.  If there is no current
    // actor (illegal use outside a spawn body), the runtime no-ops.
    auto *reason = emit_expr(*node.args[0]);
    builder.CreateCall(module->getFunction("saga_actor_trap"), {reason});
    return llvm::Constant::getNullValue(
        llvm::PointerType::getUnqual(context));
  }

  if (name == "intrinsic_syscall") {
    // intrinsic_syscall(num, args_array) → inline syscall instruction.
    // num is the syscall number (i64), args_array is an [Int] with up to
    // 6 elements.
    // On Linux x86_64 the syscall convention is:
    //   rax = syscall number
    //   rdi, rsi, rdx, r10, r8, r9 = arguments
    //   rax = return value (negative = -errno)
    auto *num = emit_expr(*node.args[0]);
    auto *arr = emit_expr(*node.args[1]);

    // Load up to 6 elements from the array. mc_array is { i64*, i64, i64 }
    // where field 0 = data ptr, field 1 = length.
    auto *arr_struct = llvm::StructType::getTypeByName(context, "mc_array");
    if (!arr_struct)
      arr_struct = llvm::StructType::create(
          context,
          {llvm::PointerType::getUnqual(context), i64_type, i64_type},
          "mc_array");
    auto *data_gep = builder.CreateStructGEP(arr_struct, arr, 0, "arr.data.ptr");
    auto *data_ptr = builder.CreateLoad(
        llvm::PointerType::getUnqual(context), data_gep, "arr.data");
    auto *len_gep = builder.CreateStructGEP(arr_struct, arr, 1, "arr.len.ptr");
    auto *len = builder.CreateLoad(i64_type, len_gep, "arr.len");

    // Load each argument with a bounds check, defaulting to 0.
    auto *zero = llvm::ConstantInt::get(i64_type, 0);
    llvm::Value *syscall_args[6];
    for (int i = 0; i < 6; ++i) {
      auto *idx = llvm::ConstantInt::get(i64_type, i);
      auto *in_bounds = builder.CreateICmpSGT(len, idx, "inb");
      auto *elem_ptr = builder.CreateGEP(i64_type, data_ptr, {idx}, "elem.ptr");
      auto *elem = builder.CreateLoad(i64_type, elem_ptr, "elem");
      syscall_args[i] = builder.CreateSelect(in_bounds, elem, zero, "arg");
    }

    // Build the inline asm for syscall.
    auto *fn_type = llvm::FunctionType::get(
        i64_type,
        {i64_type, i64_type, i64_type, i64_type, i64_type, i64_type, i64_type},
        false);
    auto *ia = llvm::InlineAsm::get(
        fn_type, "syscall",
        "={rax},{rax},{rdi},{rsi},{rdx},{r10},{r8},{r9},~{rcx},~{r11},~{memory}",
        /*hasSideEffects=*/true);
    return builder.CreateCall(
        ia, {num, syscall_args[0], syscall_args[1], syscall_args[2],
             syscall_args[3], syscall_args[4], syscall_args[5]});
  }

  if (name == "intrinsic_ptr") {
    // intrinsic_ptr(value) → load the data pointer from the backing
    // buffer.  The argument is String | [Byte] (a union). We need
    // to extract the payload pointer and then load field 0 of
    // mc_string (the data pointer).
    auto *val = emit_expr(*node.args[0]);
    auto arg_sem = semantic_type(*node.args[0]);
    llvm::Value *str_ptr = val;
    // If the arg is a union, val is a ptr to the union struct alloca.
    // Extract the payload (which is the string/array pointer).
    if (arg_sem && arg_sem->kind == TypeKind::Union) {
      auto *union_st = get_union_llvm_type(arg_sem);
      auto *payload = builder.CreateStructGEP(union_st, val, 1, "ptr.payload");
      str_ptr = builder.CreateLoad(
          llvm::PointerType::getUnqual(context), payload, "ptr.str");
    }
    auto *data_ptr = builder.CreateStructGEP(string_type, str_ptr, 0, "str.data.ptr");
    auto *ptr = builder.CreateLoad(
        llvm::PointerType::getUnqual(context), data_ptr, "str.data");
    return builder.CreatePtrToInt(ptr, i64_type, "ptr.int");
  }

  // ── Generic free-function dispatch ──────────────────────────────────
  // If the callee is a generic free function, emit (or reuse) a
  // monomorphised specialisation and call it directly.
  {
    auto callee_sem = semantic_type(*node.callee);
    if (callee_sem && callee_sem->kind == TypeKind::Func) {
      auto fd_it = analyzer.func_decl_by_type_.find(callee_sem.get());
      if (fd_it != analyzer.func_decl_by_type_.end()) {
        const FuncDeclNode *fn_decl = fd_it->second;
        if (fn_decl->generic && !fn_decl->receiver) {
          auto *ta_ptr = node_type_args_of(parent);
          if (ta_ptr) {
            auto &bindings = *ta_ptr;

            // Find the matching BodyInstantiation.
            const Analyzer::BodyInstantiation *inst = nullptr;
            auto inst_it = analyzer.instantiations_.find(fn_decl);
            if (inst_it != analyzer.instantiations_.end()) {
              for (auto &i : inst_it->second) {
                if (i.bindings.size() == bindings.size()) {
                  bool match = true;
                  for (auto &[id, t] : bindings) {
                    auto j = i.bindings.find(id);
                    if (j == i.bindings.end() ||
                        !types_equal(t, j->second)) {
                      match = false;
                      break;
                    }
                  }
                  if (match) { inst = &i; break; }
                }
              }
            }

            auto *spec = emit_specialisation(*fn_decl, callee_sem,
                                             bindings, inst);
            if (spec) {
              std::vector<llvm::Value *> args;
              for (auto &a : node.args) {
                auto *v = emit_expr(*a);
                if (v) args.push_back(v);
              }
              if (spec->getReturnType()->isVoidTy()) {
                builder.CreateCall(spec, args);
                return nullptr;
              }
              return builder.CreateCall(spec, args, "gen.call");
            }
          }
        }
      }
    }
  }

  // ── Regular function dispatch ───────────────────────────────────────
  std::string link_name;
  if (name == "intrinsic_print")
    link_name = "saga_intrinsic_print";
  else
    link_name = mangle(name);

  auto *callee = module->getFunction(link_name);

  // If not a known module function, check if it's a function-typed local
  // or parameter (first-class function value).  v1: free functions only —
  // the value is a raw LLVM function pointer; no env, no closure struct.
  if (!callee) {
    auto local_it = locals.find(name);
    if (local_it != locals.end()) {
      auto *alloca = local_it->second;
      auto callee_sem = semantic_type(*node.callee);
      bool is_func_typed = callee_sem && callee_sem->kind == TypeKind::Func;
      if (is_func_typed) {
        auto *ptr_type = llvm::PointerType::getUnqual(context);
        // The local's alloca holds a ptr; load it to get the fn pointer.
        auto *fn_ptr = builder.CreateLoad(ptr_type, alloca, "fn.load");

        std::vector<llvm::Value *> args;
        for (auto &arg_node : node.args) {
          auto *val = emit_expr(*arg_node);
          if (val)
            args.push_back(val);
        }

        std::vector<llvm::Type *> param_types;
        llvm::Type *ret_ll = void_ll_type;
        auto &fi = std::get<FuncTypeInfo>(callee_sem->detail);
        for (auto &pt : fi.params)
          param_types.push_back(llvm_type(pt));
        if (!fi.returns.empty())
          ret_ll = llvm_type(fi.returns[0]);

        auto *fn_type = llvm::FunctionType::get(ret_ll, param_types, false);
        if (ret_ll->isVoidTy()) {
          builder.CreateCall(fn_type, fn_ptr, args);
          return nullptr;
        }
        return builder.CreateCall(fn_type, fn_ptr, args, "fn.call");
      }
    }
    return nullptr;
  }

  std::vector<llvm::Value *> args;
  for (auto &arg_node : node.args) {
    auto *val = emit_expr(*arg_node);
    if (val)
      args.push_back(val);
  }

  return builder.CreateCall(callee, args);
}

// ===========================================================================
// Identifier expressions
// ===========================================================================

// ===========================================================================
// Function expressions (closures)
// ===========================================================================

llvm::Value *CodeGen::emit_func_expr(const FuncExprNode &node,
                                      const Node &parent) {
  auto *ptr_type = llvm::PointerType::getUnqual(context);
  auto *enclosing_func = builder.GetInsertBlock()->getParent();

  // Generate a unique name for this closure.
  std::string closure_name = "mc.closure." + std::to_string(next_closure_id++);

  // ── Look up captured variables ─────────────────────────────────────
  std::vector<Analyzer::CaptureInfo> captures;
  if (auto *caps = node_captures_of(parent))
    captures = *caps;

  // ── Build the environment struct type ──────────────────────────────
  // The env struct holds one field per captured variable.
  std::vector<llvm::Type *> env_field_types;
  std::vector<std::string> env_field_names;
  for (auto &cap : captures) {
    auto *ll = llvm_type(cap.type);
    env_field_types.push_back(ll);
    env_field_names.push_back(cap.name);
  }

  llvm::StructType *env_type = nullptr;
  if (!env_field_types.empty()) {
    env_type = llvm::StructType::create(context, env_field_types,
                                         closure_name + ".env");
  }

  // ── Build the trampoline function ──────────────────────────────────
  // Signature: ret_type trampoline(ptr env, param1, param2, ...)
  std::vector<llvm::Type *> tramp_param_types;
  tramp_param_types.push_back(ptr_type); // env pointer (always first)

  for (auto &param : node.signature.params) {
    auto *ll = resolve_type_node(*param.type);
    for (size_t i = 0; i < param.names.identifiers.size(); ++i)
      tramp_param_types.push_back(ll);
  }

  llvm::Type *ret_type = void_ll_type;
  if (!node.signature.returns.empty())
    ret_type = resolve_type_node(*node.signature.returns[0]);

  auto *tramp_fn_type = llvm::FunctionType::get(ret_type, tramp_param_types,
                                                  false);
  auto *tramp_fn = llvm::Function::Create(
      tramp_fn_type, llvm::Function::InternalLinkage, closure_name,
      module.get());

  // Name the arguments.
  tramp_fn->getArg(0)->setName("env");
  size_t arg_idx = 1;
  for (auto &param : node.signature.params)
    for (auto &ident : param.names.identifiers)
      if (arg_idx < tramp_fn->arg_size())
        tramp_fn->getArg(arg_idx++)->setName(std::string(ident.name));

  // ── Emit the trampoline body ───────────────────────────────────────
  // Save current insertion state.
  auto saved_block = builder.GetInsertBlock();
  auto saved_point = builder.GetInsertPoint();
  auto saved_locals = locals;
  auto saved_managed = managed_locals;
  auto saved_is_main = current_func_is_main;

  auto *entry = llvm::BasicBlock::Create(context, "entry", tramp_fn);
  builder.SetInsertPoint(entry);

  locals.clear();
  managed_locals.clear();
  current_func_is_main = false;

  // Unpack environment struct into local variables.
  if (env_type) {
    auto *env_ptr = tramp_fn->getArg(0);
    for (size_t i = 0; i < captures.size(); ++i) {
      auto *field_gep = builder.CreateStructGEP(env_type, env_ptr, i,
                                                  captures[i].name + ".cap");
      auto *field_type = env_field_types[i];
      auto *val = builder.CreateLoad(field_type, field_gep,
                                      captures[i].name);
      auto *alloca = create_entry_alloca(tramp_fn, captures[i].name,
                                          field_type);
      builder.CreateStore(val, alloca);
      locals[captures[i].name] = alloca;
    }
  }

  // Create allocas for parameters.
  arg_idx = 1;
  for (auto &param : node.signature.params) {
    auto *ll = resolve_type_node(*param.type);
    for (auto &ident : param.names.identifiers) {
      std::string pname(ident.name);
      auto *alloca = create_entry_alloca(tramp_fn, pname, ll);
      builder.CreateStore(tramp_fn->getArg(arg_idx++), alloca);
      locals[pname] = alloca;
    }
  }

  // Emit the closure body.
  auto &block = std::get<BlockNode>(node.body->data);
  auto *tail_val = emit_block(block);

  // Add terminator if needed.
  if (!builder.GetInsertBlock()->getTerminator()) {
    emit_release_locals();
    if (ret_type->isVoidTy()) {
      builder.CreateRetVoid();
    } else if (tail_val && tail_val->getType() == ret_type) {
      builder.CreateRet(tail_val);
    } else {
      builder.CreateRet(llvm::Constant::getNullValue(ret_type));
    }
  }

  llvm::verifyFunction(*tramp_fn);

  // ── Restore the enclosing function's state ─────────────────────────
  builder.SetInsertPoint(saved_block, saved_point);
  locals = saved_locals;
  managed_locals = saved_managed;
  current_func_is_main = saved_is_main;

  // ── Allocate and populate the environment struct ───────────────────
  llvm::Value *env_ptr = llvm::ConstantPointerNull::get(ptr_type);
  if (env_type) {
    auto *env_alloca = create_entry_alloca(enclosing_func,
                                            closure_name + ".env",
                                            env_type);
    // Store each captured variable's current value into the env.
    for (size_t i = 0; i < captures.size(); ++i) {
      auto *field_gep = builder.CreateStructGEP(env_type, env_alloca, i,
                                                  captures[i].name + ".env.f");
      // Load the current value of the captured variable.
      auto local_it = locals.find(captures[i].name);
      if (local_it != locals.end()) {
        auto *alloca = local_it->second;
        auto *val = builder.CreateLoad(env_field_types[i], alloca,
                                        captures[i].name + ".cap.val");
        builder.CreateStore(val, field_gep);
      } else {
        // Zero-initialize if not found (shouldn't happen after analysis).
        builder.CreateStore(
            llvm::Constant::getNullValue(env_field_types[i]), field_gep);
      }
    }
    env_ptr = env_alloca;
  }

  // ── Build the closure fat pointer { fn_ptr, env_ptr } ─────────────
  auto *closure_alloca = create_entry_alloca(enclosing_func,
                                              closure_name + ".val",
                                              closure_fat_ptr_type);
  auto *fn_gep = builder.CreateStructGEP(closure_fat_ptr_type,
                                          closure_alloca, 0, "closure.fn");
  builder.CreateStore(tramp_fn, fn_gep);
  auto *env_gep = builder.CreateStructGEP(closure_fat_ptr_type,
                                            closure_alloca, 1, "closure.env");
  builder.CreateStore(env_ptr, env_gep);

  return closure_alloca;
}

// ===========================================================================
// Spawn expression
// ===========================================================================

llvm::Value *CodeGen::emit_spawn_expr(const SpawnExprNode &node,
                                       const Node &parent) {
  has_spawn = true;

  auto *ptr_type = llvm::PointerType::getUnqual(context);
  auto *enclosing_func = builder.GetInsertBlock()->getParent();

  // Generate a unique name for this spawn's outlined function.
  std::string spawn_name = "mc.spawn." + std::to_string(next_spawn_id++);

  // ── Look up captured variables ─────────────────────────────────────
  std::vector<Analyzer::SpawnCaptureInfo> captures;
  if (auto *caps = spawn_captures_of(parent))
    captures = *caps;

  // ── Build the closure struct type ──────────────────────────────────
  // Holds one field per captured variable.
  std::vector<llvm::Type *> closure_field_types;
  std::vector<std::string> closure_field_names;
  for (auto &cap : captures) {
    auto *ll = llvm_type(cap.type);
    closure_field_types.push_back(ll);
    closure_field_names.push_back(cap.name);
  }

  llvm::StructType *closure_st = nullptr;
  uint64_t closure_size = 0;
  if (!closure_field_types.empty()) {
    closure_st = llvm::StructType::create(context, closure_field_types,
                                           spawn_name + ".closure");
    closure_size = module->getDataLayout().getTypeAllocSize(closure_st);
  }

  // ── Build the outlined function: void(mc_actor*) ───────────────────
  auto *outlined_fn_type = llvm::FunctionType::get(
      void_ll_type, {ptr_type}, false);
  auto *outlined_fn = llvm::Function::Create(
      outlined_fn_type, llvm::Function::InternalLinkage, spawn_name,
      module.get());
  outlined_fn->getArg(0)->setName("actor");

  // ── Emit the outlined function body ────────────────────────────────
  auto saved_block = builder.GetInsertBlock();
  auto saved_point = builder.GetInsertPoint();
  auto saved_locals = locals;
  auto saved_managed = managed_locals;
  auto saved_is_main = current_func_is_main;
  auto saved_actor = current_actor;

  auto *entry = llvm::BasicBlock::Create(context, "entry", outlined_fn);
  builder.SetInsertPoint(entry);

  locals.clear();
  managed_locals.clear();
  current_func_is_main = false;
  current_actor = outlined_fn->getArg(0);

  // Unpack closure data from actor->closure_data.
  // The runtime copies the closure struct into the actor's arena, so we
  // just read it back from the pointer stored in the mc_actor.  However,
  // we don't have the C struct layout in LLVM IR, so the runtime passes
  // the closure_data pointer.  We store it in a local and GEP into the
  // closure struct type.
  //
  // The mc_actor struct has closure_data at a known offset.  We'll use
  // a dedicated runtime helper or a fixed byte offset.  Since mc_actor
  // is opaque to codegen, we pass the closure_data as the entry arg's
  // associated data.  The runtime stores it in actor->closure_data and
  // the worker loop calls entry(actor).  We load it via:
  //   closure_data = *(void**)(actor + offsetof(mc_actor, closure_data))
  //
  // offsetof(mc_actor, closure_data) needs to match the C struct.
  // mc_actor layout (64-bit): refcount(8) result(8) result_size(8)
  //   status(8) cancelled(8) lock(40) done_cond(48) arena(8)
  //   entry(8) closure_data(8) ...
  // That's fragile.  Instead, generate a tiny C helper.
  //
  // Simpler approach: add a runtime function mc_actor_closure_data(actor)
  // that returns actor->closure_data.
  //
  // For now, let's define the offset based on the mc_actor C struct.
  // A more robust approach: declare mc_actor_closure_data as a runtime fn.
  //
  // Actually, the cleanest approach: we already have the closure_data
  // pointer passed via mc_actor_new.  The outlined function receives the
  // actor*, and the closure_data was memcpy'd into the arena.
  // We need to access it.  Let's add a trivial runtime accessor.

  if (closure_st && !captures.empty()) {
    // Declare or get the accessor: void* mc_actor_get_closure(mc_actor*)
    auto *accessor = module->getFunction("mc_actor_get_closure");
    if (!accessor) {
      accessor = llvm::Function::Create(
          llvm::FunctionType::get(ptr_type, {ptr_type}, false),
          llvm::Function::ExternalLinkage, "mc_actor_get_closure",
          module.get());
    }

    auto *closure_ptr = builder.CreateCall(accessor, {current_actor},
                                            "closure.ptr");

    for (size_t i = 0; i < captures.size(); ++i) {
      auto *field_gep = builder.CreateStructGEP(
          closure_st, closure_ptr, i, captures[i].name + ".cap");
      auto *val = builder.CreateLoad(closure_field_types[i], field_gep,
                                      captures[i].name);
      auto *alloca = create_entry_alloca(outlined_fn, captures[i].name,
                                          closure_field_types[i]);
      builder.CreateStore(val, alloca);
      locals[captures[i].name] = alloca;

      // Retain shared refcounted captures.
      if (captures[i].kind == Analyzer::SpawnCaptureKind::Share) {
        // The runtime already memcpy'd the data, but we need to retain
        // the refcounted objects so they aren't freed while we use them.
        TypePtr cap_type = captures[i].type;
        emit_retain(val, cap_type);
      }
    }
  }

  // If the spawn has a pipe variable (|task|), bind it to the actor ptr.
  if (node.pipe) {
    std::string pipe_name(node.pipe->name);
    auto *alloca = create_entry_alloca(outlined_fn, pipe_name, ptr_type);
    builder.CreateStore(current_actor, alloca);
    locals[pipe_name] = alloca;
  }

  // Emit the spawn body.
  if (auto *block = std::get_if<BlockNode>(&node.body->data)) {
    emit_block(*block);
  } else {
    // Identifier body (function reference) — call it as a trampoline.
    auto *body_val = emit_expr(*node.body);
    // If it returned a value, we could use context_exit, but for
    // simplicity we just ignore it; the function should use task.Exit().
    (void)body_val;
  }

  // Add terminator if needed.
  if (!builder.GetInsertBlock()->getTerminator())
    builder.CreateRetVoid();

  llvm::verifyFunction(*outlined_fn);

  // ── Restore enclosing function state ───────────────────────────────
  builder.SetInsertPoint(saved_block, saved_point);
  locals = saved_locals;
  managed_locals = saved_managed;
  current_func_is_main = saved_is_main;
  current_actor = saved_actor;

  // ── Pack closure data at the spawn site ────────────────────────────
  llvm::Value *closure_ptr_val =
      llvm::ConstantPointerNull::get(ptr_type);

  if (closure_st && !captures.empty()) {
    auto *closure_alloca = create_entry_alloca(
        enclosing_func, spawn_name + ".closure", closure_st);

    for (size_t i = 0; i < captures.size(); ++i) {
      auto *field_gep = builder.CreateStructGEP(
          closure_st, closure_alloca, i, captures[i].name + ".pack");

      auto local_it = locals.find(captures[i].name);
      if (local_it != locals.end()) {
        auto *alloca = local_it->second;
        auto *val = builder.CreateLoad(closure_field_types[i], alloca,
                                        captures[i].name + ".cap.val");
        builder.CreateStore(val, field_gep);

        // Retain shared refcounted captures at the spawn site.
        if (captures[i].kind == Analyzer::SpawnCaptureKind::Share)
          emit_retain(val, captures[i].type);
      } else {
        builder.CreateStore(
            llvm::Constant::getNullValue(closure_field_types[i]),
            field_gep);
      }
    }
    closure_ptr_val = closure_alloca;
  }

  // ── Create channel if generic type present ─────────────────────────
  llvm::Value *channel_ptr = nullptr;
  int64_t channel_elem_size = 0;
  if (node.generic && !node.generic->type_params.empty()) {
    // Read the channel element type the analyzer resolved while the
    // spawn's enclosing scope was still live.  Re-resolving here would
    // fail for user struct names because their scope has been popped.
    TypePtr chan_elem_sem = spawn_channel_elem_type_of(parent);
    if (!chan_elem_sem)
      chan_elem_sem = analyzer.resolve_type(*node.generic->type_params[0]);
    auto *chan_elem_ll = llvm_type(chan_elem_sem);
    if (!chan_elem_ll || chan_elem_ll->isVoidTy())
      chan_elem_ll = i64_type; // defensive fallback; shouldn't happen
    channel_elem_size = static_cast<int64_t>(
        module->getDataLayout().getTypeAllocSize(chan_elem_ll));

    auto *ch = builder.CreateCall(
        module->getFunction("saga_channel_new"),
        {llvm::ConstantInt::get(i64_type, channel_elem_size),
         llvm::ConstantInt::get(i64_type, 0)}, // 0 = default capacity
        "channel");
    channel_ptr = ch;
  }

  // ── Spawn the actor ────────────────────────────────────────────────
  auto *actor = builder.CreateCall(
      module->getFunction("saga_executor_spawn"),
      {outlined_fn,
       closure_ptr_val,
       llvm::ConstantInt::get(i64_type, static_cast<int64_t>(closure_size)),
       llvm::ConstantInt::get(i64_type, 0)}, // 0 = default arena max
      "actor");

  // Attach channel to actor if present.
  // We need to store ch into actor->channel.  Use a runtime helper.
  if (channel_ptr) {
    auto *attach_fn = module->getFunction("mc_actor_set_channel");
    if (!attach_fn) {
      attach_fn = llvm::Function::Create(
          llvm::FunctionType::get(void_ll_type, {ptr_type, ptr_type}, false),
          llvm::Function::ExternalLinkage, "mc_actor_set_channel",
          module.get());
    }
    builder.CreateCall(attach_fn, {actor, channel_ptr});
  }

  // ── Store the actor ptr as the Task handle ─────────────────────────
  // The actor ptr IS the Task — mc_actor* is our runtime Task handle.
  // Store it in a local alloca (caller will bind it via DeclAssign).

  // If there's a channel, also store it as a companion local so the
  // for-range iteration can find it.  The DeclAssign handler will bind
  // the task name, and we store the channel under "<name>.channel".
  // Since we don't know the target name yet, we use a temporary.
  // The channel is returned alongside the actor via a small wrapper.

  // For channel-based spawns, store the channel ptr in a pseudo-local
  // that the for-range handler can look up.
  if (channel_ptr) {
    // We'll store it with a predictable name.  The DeclAssign that
    // receives this spawn result will rename the actor alloca.  We
    // stash the channel in a local keyed by the spawn id.
    std::string ch_local = spawn_name + ".channel";
    auto *ch_alloca = create_entry_alloca(enclosing_func, ch_local, ptr_type);
    builder.CreateStore(channel_ptr, ch_alloca);
    locals[ch_local] = ch_alloca;

    // Also, we'll need to make the DeclAssign handler aware.
    // Store a mapping from the actor value to the channel local name.
    // For simplicity, we use a side-channel: after the DeclAssign binds
    // the task name, we create a "<taskname>.channel" alias.
    // We handle this by storing the channel under a known key and
    // having a post-decl-assign hook.  Instead, let's just store the
    // channel alloca indexed by the actor pointer value — but that's
    // fragile with LLVM values.
    //
    // Simplest approach: return a tagged pair.  But since our codegen
    // returns a single llvm::Value*, we need another mechanism.
    //
    // Pragmatic solution: stash the pending channel in a member variable.
    // The next DeclAssign that receives this actor ptr will pick it up.
    pending_channel_alloca_ = ch_alloca;
  }

  return actor;
}

llvm::Value *CodeGen::emit_identifier(const IdentifierNode &node) {
  std::string name(node.name);

  // Check local variables.
  auto it = locals.find(name);
  if (it != locals.end()) {
    // Union types are passed as pointers to their tagged struct.
    // Return the alloca directly rather than loading.
    auto *alloca = it->second;
    auto *alloc_ty = alloca->getAllocatedType();
    if (alloc_ty->isStructTy()) {
      auto *st = llvm::cast<llvm::StructType>(alloc_ty);
      // Check if this is a union struct: { i8, [N x i8] }
      if (st->getNumElements() == 2 &&
          st->getElementType(0)->isIntegerTy(8) &&
          st->getElementType(1)->isArrayTy()) {
        return alloca; // Return pointer to union struct.
      }
      // Closure fat pointer — return the alloca pointer.
      if (st == closure_fat_ptr_type) {
        return alloca;
      }
    }
    return builder.CreateLoad(alloc_ty, alloca, name);
  }

  // Builtin constants.
  if (name == "true")
    return llvm::ConstantInt::get(i1_type, 1);
  if (name == "false")
    return llvm::ConstantInt::get(i1_type, 0);

  // Enum type names — return a sentinel so selectors can access variants.
  if (enum_types.count(name))
    return llvm::ConstantInt::get(i64_type, 0);

  // Top-level function referenced as a value (e.g. `call_it(greet, ...)` or
  // a struct literal like `Reg{ handler: greet }`).  Return the raw LLVM
  // Function* — it is pointer-typed, matching how function-typed locals
  // and struct fields are lowered.
  if (auto *fn = module->getFunction(mangle(name)))
    return fn;

  return nullptr;
}

} // namespace mc

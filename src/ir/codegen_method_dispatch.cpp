// Copyright 2026 Rob Thornton
// SPDX-License-Identifier: MIT

// Selector-callee call emission: dispatch when the callee of a CallExpr
// is a selector (`obj.method(args)`). Covers module function calls,
// struct method dispatch, struct-field function calls (closures stored
// in fields), task message handlers, and context methods inside spawned
// actors. Direct (identifier) call emission stays in codegen_calls.cpp.

#include "ir/codegen.hpp"

#include <algorithm>
#include <llvm/IR/Constants.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Verifier.h>

namespace mc {

llvm::Value *CodeGen::emit_method_or_module_call(const CallExprNode &node,
                                                 const Node &parent) {
  auto *sel = std::get_if<SelectorNode>(&node.callee->data);
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

    // Sret lowering: if callee returns a struct via sret, alloca the
    // result struct and pass as the hidden first argument.
    auto *parent_fn = builder.GetInsertBlock()->getParent();
    llvm::Value *sret_slot = nullptr;
    llvm::Type *sret_struct_ty = nullptr;
    if (callee->arg_size() > 0 && callee->getArg(0)->hasStructRetAttr()) {
      sret_struct_ty = callee->getParamStructRetType(0);
      sret_slot = create_entry_alloca(parent_fn, "sret.tmp", sret_struct_ty);
      args.push_back(sret_slot);
    }

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
        // concrete, wrap the value into the union struct (yields a ptr
        // to the union alloca).
        if (i < fn_info.params.size() &&
            fn_info.params[i]->kind == TypeKind::Union) {
          auto arg_sem = semantic_type(*node.args[i]);
          if (arg_sem && arg_sem->kind != TypeKind::Union) {
            auto *wrapped = emit_union_wrap(val, arg_sem, fn_info.params[i]);
            if (wrapped)
              val = wrapped; // keep as pointer for byval lowering
          }
        }
        // Interface boxing: param expects an interface, arg is a concrete
        // struct.  Spill struct SSA values, then wrap the struct pointer
        // in a fat pointer { data, vtable }.
        if (i < fn_info.params.size() && fn_info.params[i] &&
            fn_info.params[i]->kind == TypeKind::Interface) {
          auto arg_sem = semantic_type(*node.args[i]);
          if (arg_sem && arg_sem->kind == TypeKind::Struct) {
            llvm::Value *struct_ptr = val;
            if (val->getType()->isStructTy()) {
              auto *p_ll = llvm_type(arg_sem);
              auto *tmp = create_entry_alloca(parent_fn, "iface.arg.spill",
                                               p_ll);
              builder.CreateStore(val, tmp);
              struct_ptr = tmp;
            }
            auto *boxed = emit_interface_box(struct_ptr, arg_sem,
                                              fn_info.params[i]);
            if (boxed)
              val = boxed;
          }
        }
        // Byval struct/union param: ensure val is a pointer to the
        // struct/union alloca.  If it's an SSA struct value, spill.
        if (i < fn_info.params.size() && fn_info.params[i] &&
            (fn_info.params[i]->kind == TypeKind::Struct ||
             fn_info.params[i]->kind == TypeKind::Union)) {
          auto *p_ll = llvm_type(fn_info.params[i]);
          if (p_ll && p_ll->isStructTy()) {
            if (val->getType()->isStructTy()) {
              auto *tmp = create_entry_alloca(parent_fn, "arg.spill",
                                               p_ll);
              builder.CreateStore(val, tmp);
              val = tmp;
            }
            // else val is already a pointer to a struct alloca.
          }
        }
        args.push_back(val);
      }
    }

    auto *call = builder.CreateCall(callee, args,
        callee->getReturnType()->isVoidTy() ? "" : "pkg.call");

    // Mirror sret/byval attrs on the call site so LLVM lowers correctly.
    unsigned idx = 0;
    if (sret_slot) {
      call->addParamAttr(idx,
          llvm::Attribute::getWithStructRetType(context, sret_struct_ty));
      call->addParamAttr(idx,
          llvm::Attribute::getWithAlignment(context,
              module->getDataLayout().getABITypeAlign(sret_struct_ty)));
      ++idx;
    }
    for (size_t i = 0; i < fn_info.params.size(); ++i) {
      if (fn_info.params[i] &&
          (fn_info.params[i]->kind == TypeKind::Struct ||
           fn_info.params[i]->kind == TypeKind::Union)) {
        auto *p_ll = llvm_type(fn_info.params[i]);
        if (p_ll && p_ll->isStructTy()) {
          call->addParamAttr(idx,
              llvm::Attribute::getWithByValType(context, p_ll));
          call->addParamAttr(idx,
              llvm::Attribute::getWithAlignment(context,
                  module->getDataLayout().getABITypeAlign(p_ll)));
        }
      }
      ++idx;
    }

    if (sret_slot)
      return sret_slot;
    if (callee->getReturnType()->isVoidTy())
      return nullptr;
    return call;
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
          auto st_it = struct_types.find(key_for(sinfo.origin_package, sinfo.name));
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

    // Generic method on a concrete struct: monomorphise at call site.
    for (auto &m : info.methods) {
      if (m.name == method && m.signature && has_type_params(m.signature)) {
        auto fd_it = analyzer.func_decl_by_type_.find(m.signature.get());
        if (fd_it != analyzer.func_decl_by_type_.end()) {
          auto *ta_ptr = node_type_args_of(parent);
          if (ta_ptr) {
            auto &bindings = *ta_ptr;
            const Analyzer::BodyInstantiation *inst = nullptr;
            auto inst_it = analyzer.instantiations_.find(fd_it->second);
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
            auto *spec = emit_specialisation(
                *fd_it->second, m.signature, bindings, inst);
            if (spec) {
              std::vector<llvm::Value *> args;
              // self pointer
              if (auto *id = std::get_if<IdentifierNode>(
                      &sel->object->data)) {
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
          }
        }
        break;
      }
    }

    // Use the struct's origin_package to form the qualified key and link name.
    // Falls back to package_name for local types with empty origin_package.
    std::string struct_origin =
        info.origin_package.empty() ? package_name : info.origin_package;
    std::string struct_key = mangle(struct_origin, info.name);
    std::string link_name = mangle(struct_origin, info.name + "__" + method);
    auto *callee = module->getFunction(link_name);

    // If the method isn't forward-declared yet (shouldn't happen after
    // materialize_import, but guard for non-imported local types), declare it.
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
      auto *parent_fn = builder.GetInsertBlock()->getParent();

      // Sret lowering: if callee returns a struct via sret, alloca a
      // result slot and pass it as the hidden first argument.
      std::vector<llvm::Value *> args;
      llvm::Value *sret_slot = nullptr;
      llvm::Type *sret_struct_ty = nullptr;
      if (callee->arg_size() > 0 && callee->getArg(0)->hasStructRetAttr()) {
        sret_struct_ty = callee->getParamStructRetType(0);
        sret_slot = create_entry_alloca(parent_fn, "sret.tmp", sret_struct_ty);
        args.push_back(sret_slot);
      }

      // For struct methods, self is a pointer to the struct.
      llvm::Value *self_ptr = obj;
      if (auto *id = std::get_if<IdentifierNode>(&sel->object->data)) {
        auto local_it = locals.find(std::string(id->name));
        if (local_it != locals.end()) {
          auto *alloca = local_it->second;
          auto *alloca_type = alloca->getAllocatedType();
          auto st_it = struct_types.find(struct_key);
          if (st_it != struct_types.end() && alloca_type == st_it->second) {
            self_ptr = alloca;
          } else if (alloca_type->isPointerTy()) {
            self_ptr = builder.CreateLoad(alloca_type, alloca, "self.ptr");
          }
        }
      }

      // If self_ptr is a struct value (not a pointer/alloca), spill.
      auto st_it2 = struct_types.find(struct_key);
      if (st_it2 != struct_types.end() &&
          self_ptr->getType() == st_it2->second) {
        auto *tmp = create_entry_alloca(parent_fn, "self.tmp",
                                         st_it2->second);
        builder.CreateStore(self_ptr, tmp);
        self_ptr = tmp;
      }
      args.push_back(self_ptr);

      // Resolve the method signature for byval lowering of struct args.
      const FuncTypeInfo *m_fi = nullptr;
      for (auto &m : info.methods) {
        if (m.name == method && m.signature &&
            m.signature->kind == TypeKind::Func) {
          m_fi = &std::get<FuncTypeInfo>(m.signature->detail);
          break;
        }
      }

      for (size_t ai = 0; ai < node.args.size(); ++ai) {
        auto *val = emit_expr(*node.args[ai]);
        if (!val)
          continue;
        if (m_fi && ai < m_fi->params.size() && m_fi->params[ai] &&
            (m_fi->params[ai]->kind == TypeKind::Struct ||
             m_fi->params[ai]->kind == TypeKind::Union)) {
          auto *p_ll = llvm_type(m_fi->params[ai]);
          if (p_ll && p_ll->isStructTy() && val->getType()->isStructTy()) {
            auto *tmp = create_entry_alloca(parent_fn, "arg.spill", p_ll);
            builder.CreateStore(val, tmp);
            val = tmp;
          }
        }
        args.push_back(val);
      }

      std::string call_name =
          callee->getReturnType()->isVoidTy() ? "" : "mcall";
      auto *call = builder.CreateCall(callee, args, call_name);

      // Mirror sret/byval attrs on the call site.
      unsigned cidx = 0;
      if (sret_slot) {
        call->addParamAttr(cidx,
            llvm::Attribute::getWithStructRetType(context, sret_struct_ty));
        call->addParamAttr(cidx,
            llvm::Attribute::getWithAlignment(context,
                module->getDataLayout().getABITypeAlign(sret_struct_ty)));
        ++cidx;
      }
      ++cidx; // self
      if (m_fi) {
        for (size_t ai = 0; ai < m_fi->params.size(); ++ai) {
          if (m_fi->params[ai] &&
              (m_fi->params[ai]->kind == TypeKind::Struct ||
               m_fi->params[ai]->kind == TypeKind::Union)) {
            auto *p_ll = llvm_type(m_fi->params[ai]);
            if (p_ll && p_ll->isStructTy()) {
              call->addParamAttr(cidx,
                  llvm::Attribute::getWithByValType(context, p_ll));
              call->addParamAttr(cidx,
                  llvm::Attribute::getWithAlignment(context,
                      module->getDataLayout().getABITypeAlign(p_ll)));
            }
          }
          ++cidx;
        }
      }

      if (sret_slot)
        return sret_slot;
      if (callee->getReturnType()->isVoidTy())
        return nullptr;
      return call;
    }
  }

  // Interface dynamic dispatch: obj.Method(args) via vtable.
  if (obj_sem && obj_sem->kind == TypeKind::Interface) {
    auto &iface_info = std::get<InterfaceTypeInfo>(obj_sem->detail);
    // key_for falls back to package_name for local types; bare "Error" stays
    // bare since its origin_package is empty and the built-in uses bare key.
    std::string iface_key = key_for(iface_info.origin_package, iface_info.name);

    auto mn_it = iface_method_names.find(iface_key);
    auto vt_it = iface_vtable_types.find(iface_key);
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

} // namespace mc

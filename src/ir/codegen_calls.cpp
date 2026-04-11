// Copyright 2026 Rob Thornton
// SPDX-License-Identifier: MIT

#include "ir/codegen.hpp"

#include <llvm/IR/Constants.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/InlineAsm.h>
#include <llvm/IR/Verifier.h>

namespace mc {

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

llvm::Value *CodeGen::emit_call_expr(const CallExprNode &node) {
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
            module->getFunction("mc_array_new"),
            {llvm::ConstantInt::get(i64_type, 8),
             llvm::ConstantInt::get(i64_type, var_count)}, "varargs");
        auto *func = builder.GetInsertBlock()->getParent();
        for (size_t i = 0; i < var_count; ++i) {
          auto *val = emit_expr(*node.args[fixed_count + i]);
          if (!val) continue;
          auto *tmp = create_entry_alloca(func, "va.tmp", val->getType());
          builder.CreateStore(val, tmp);
          builder.CreateCall(module->getFunction("mc_array_push"),
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

    auto *obj = emit_expr(*sel->object);
    if (!obj)
      return nullptr;

    // Array methods.
    if (obj_sem && obj_sem->kind == TypeKind::Array) {
      if (method == "Size") {
        auto *size_fn = module->getFunction("mc_array_size");
        return builder.CreateCall(size_fn, {obj}, "size");
      }
      if (method == "Push" && !node.args.empty()) {
        auto *val = emit_expr(*node.args[0]);
        if (!val) return nullptr;
        auto *func = builder.GetInsertBlock()->getParent();
        auto *tmp = create_entry_alloca(func, "push.tmp", val->getType());
        builder.CreateStore(val, tmp);
        auto *push_fn = module->getFunction("mc_array_push");
        builder.CreateCall(push_fn, {obj, tmp});
        return obj; // Push returns the array.
      }
    }

    // Map methods.
    if (obj_sem && obj_sem->kind == TypeKind::Map) {
      auto &map_info = std::get<MapTypeInfo>(obj_sem->detail);
      bool string_keys = is_string_key_type(map_info.key);
      auto *is_str_key = llvm::ConstantInt::get(i64_type, string_keys ? 1 : 0);

      if (method == "Size") {
        auto *size_fn = module->getFunction("mc_map_size");
        return builder.CreateCall(size_fn, {obj}, "map.size");
      }
      if (method == "Set" && node.args.size() >= 2) {
        auto *key_val = emit_expr(*node.args[0]);
        auto *val_val = emit_expr(*node.args[1]);
        if (!key_val || !val_val) return nullptr;
        auto *func = builder.GetInsertBlock()->getParent();
        auto *key_tmp = create_entry_alloca(func, "map.set.key", key_val->getType());
        builder.CreateStore(key_val, key_tmp);
        auto *val_tmp = create_entry_alloca(func, "map.set.val", val_val->getType());
        builder.CreateStore(val_val, val_tmp);
        auto *set_fn = module->getFunction("mc_map_set");
        builder.CreateCall(set_fn, {obj, key_tmp, val_tmp, is_str_key});
        return nullptr;
      }
      if (method == "At" && node.args.size() >= 1) {
        auto *key_val = emit_expr(*node.args[0]);
        if (!key_val) return nullptr;
        auto *func = builder.GetInsertBlock()->getParent();
        auto *key_tmp = create_entry_alloca(func, "map.at.key", key_val->getType());
        builder.CreateStore(key_val, key_tmp);
        auto *get_fn = module->getFunction("mc_map_get");
        auto *val_ptr = builder.CreateCall(get_fn, {obj, key_tmp, is_str_key}, "map.at");
        auto *val_ll = llvm_type(map_info.value);
        return builder.CreateLoad(val_ll, val_ptr, "map.at.val");
      }
      if ((method == "Key?" || method == "Has") && node.args.size() >= 1) {
        auto *key_val = emit_expr(*node.args[0]);
        if (!key_val) return nullptr;
        auto *func = builder.GetInsertBlock()->getParent();
        auto *key_tmp = create_entry_alloca(func, "map.has.key", key_val->getType());
        builder.CreateStore(key_val, key_tmp);
        auto *has_fn = module->getFunction("mc_map_has");
        auto *result = builder.CreateCall(has_fn, {obj, key_tmp, is_str_key}, "map.has");
        return builder.CreateICmpNE(result, llvm::ConstantInt::get(i64_type, 0), "map.has.bool");
      }
      if (method == "Remove" && node.args.size() >= 1) {
        auto *key_val = emit_expr(*node.args[0]);
        if (!key_val) return nullptr;
        auto *func = builder.GetInsertBlock()->getParent();
        auto *key_tmp = create_entry_alloca(func, "map.rm.key", key_val->getType());
        builder.CreateStore(key_val, key_tmp);
        auto *rm_fn = module->getFunction("mc_map_remove");
        builder.CreateCall(rm_fn, {obj, key_tmp, is_str_key});
        return nullptr;
      }
    }

    // String methods.
    if (obj_sem && obj_sem->kind == TypeKind::String) {
      if (method == "Size") {
        // String size = load the len field from mc_string.
        auto *len_gep = builder.CreateStructGEP(string_type, obj, 1, "len.ptr");
        return builder.CreateLoad(i64_type, len_gep, "len");
      }
    }

    // Enum .Int() — enums are already i64 index values in LLVM IR.
    if (method == "Int" && obj_sem && obj_sem->kind == TypeKind::Enum) {
      return obj;
    }

    // Enum .String() — convert the enum's integer index to a string.
    if (method == "String" && obj_sem && obj_sem->kind == TypeKind::Enum) {
      return builder.CreateCall(
          module->getFunction("mc_int_to_string"), {obj}, "str");
    }

    // Int/Float/Bool .String() method — convert to string.
    if (method == "String" && obj_sem) {
      if (obj_sem->kind == TypeKind::Int) {
        return builder.CreateCall(
            module->getFunction("mc_int_to_string"), {obj}, "str");
      }
      if (obj_sem->kind == TypeKind::Float) {
        return builder.CreateCall(
            module->getFunction("mc_float_to_string"), {obj}, "str");
      }
      if (obj_sem->kind == TypeKind::Bool) {
        auto *ext = builder.CreateZExt(obj, i64_type, "bext");
        return builder.CreateCall(
            module->getFunction("mc_bool_to_string"), {ext}, "str");
      }
    }

    // ── Task method calls ─────────────────────────────────────────────
    // Task is a semantic struct wrapping mc_actor*.  obj is the actor ptr.
    if (obj_sem && obj_sem->kind == TypeKind::Struct) {
      auto &sinfo = std::get<StructTypeInfo>(obj_sem->detail);
      if (sinfo.name == "Task") {
        if (method == "Alive?") {
          auto *fn = module->getFunction("mc_task_alive");
          auto *result = builder.CreateCall(fn, {obj}, "alive");
          return builder.CreateICmpNE(result,
              llvm::ConstantInt::get(i64_type, 0), "alive.bool");
        }
        if (method == "Cancel") {
          builder.CreateCall(module->getFunction("mc_task_cancel"), {obj});
          return nullptr;
        }
        if (method == "Term") {
          builder.CreateCall(module->getFunction("mc_task_term"), {obj});
          return nullptr;
        }
        if (method == "Wait") {
          auto *func = builder.GetInsertBlock()->getParent();
          auto *status_alloca = create_entry_alloca(func, "wait.status",
                                                     i64_type);
          builder.CreateStore(llvm::ConstantInt::get(i64_type, 0),
                              status_alloca);
          auto *result_ptr = builder.CreateCall(
              module->getFunction("mc_task_wait"),
              {obj, status_alloca}, "wait.result");
          // Load the status to check for error.
          auto *status = builder.CreateLoad(i64_type, status_alloca,
                                             "wait.status");
          // MC_ACTOR_COMPLETED == 2
          auto *is_ok = builder.CreateICmpEQ(status,
              llvm::ConstantInt::get(i64_type, 2), "wait.ok");

          // The result type depends on the generic type parameter.
          // For now return the raw pointer — the or-expr handler will
          // wrap it into a union if needed.  If the parent expects a
          // specific type, the result_ptr points to a copy of it.
          // We return result_ptr as an opaque ptr.
          return result_ptr;
        }
        // Fall through to generic struct method handling if not matched.
      }

      // ── Context method calls (inside spawn body) ─────────────────
      if (sinfo.name == "Context") {
        if (method == "Cancelled?") {
          auto *fn = module->getFunction("mc_context_cancelled");
          auto *result = builder.CreateCall(fn, {obj}, "cancelled");
          return builder.CreateICmpNE(result,
              llvm::ConstantInt::get(i64_type, 0), "cancelled.bool");
        }
        if (method == "Send" && !node.args.empty()) {
          auto *val = emit_expr(*node.args[0]);
          if (!val) return nullptr;
          auto *func = builder.GetInsertBlock()->getParent();
          auto *tmp = create_entry_alloca(func, "send.tmp", val->getType());
          builder.CreateStore(val, tmp);
          builder.CreateCall(module->getFunction("mc_context_send"),
                             {obj, tmp});
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
                  module->getFunction("mc_context_exit"),
                  {obj, tmp,
                   llvm::ConstantInt::get(i64_type, sz)});
            }
          } else {
            auto *null_ptr = llvm::ConstantPointerNull::get(
                llvm::PointerType::getUnqual(context));
            builder.CreateCall(
                module->getFunction("mc_context_exit"),
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
      const Type *raw = obj_sem.get();
      auto tm_it = analyzer.type_methods_.find(raw);
      if (tm_it != analyzer.type_methods_.end()) {
        for (auto &m : tm_it->second) {
          if (m.name != method)
            continue;
          // Determine the origin package from intrinsic_method_links,
          // or fall back to a well-known package name from the TypeKind.
          const char *type_name = nullptr;
          switch (obj_sem->kind) {
          case TypeKind::Int:    type_name = "Int"; break;
          case TypeKind::Float:  type_name = "Float"; break;
          case TypeKind::Bool:   type_name = "Bool"; break;
          case TypeKind::String: type_name = "String"; break;
          default: break;
          }
          if (!type_name)
            break;

          // Look for the function in intrinsic_method_links (same module)
          // or declare it as external.
          std::string local_link =
              mangle(std::string(type_name) + "__" + method);
          auto *callee = module->getFunction(local_link);
          if (!callee && m.signature &&
              m.signature->kind == TypeKind::Func) {
            // Cross-package: find the origin package that defines this
            // method.  Scan all known intrinsic_method_links.
            std::string origin_pkg;
            for (auto &[tname, links] : intrinsic_method_links) {
              if (tname == type_name) {
                for (auto &[ln, mn] : links) {
                  if (mn == method) {
                    origin_pkg = package_name; // same package
                    break;
                  }
                }
              }
              if (!origin_pkg.empty())
                break;
            }
            // If not found locally, the method must come from a stdlib
            // package compiled separately. Declare an external function.
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
                ft, llvm::Function::ExternalLinkage, local_link,
                module.get());
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

  if (name == "intrinsic_yield") {
    // intrinsic_yield() → mc_actor_yield(current_actor)
    if (current_actor) {
      builder.CreateCall(module->getFunction("mc_actor_yield"),
                         {current_actor});
    }
    // Outside a spawn block this is a no-op.
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
    // intrinsic_trap(reason) → mc_actor_trap(current_actor, reason)
    auto *reason = emit_expr(*node.args[0]);
    if (current_actor) {
      builder.CreateCall(module->getFunction("mc_actor_trap"),
                         {current_actor, reason});
    }
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

  // ── Regular function dispatch ───────────────────────────────────────
  std::string link_name;
  if (name == "intrinsic_print")
    link_name = "mc_intrinsic_print";
  else
    link_name = mangle(name);

  auto *callee = module->getFunction(link_name);

  // If not a known module function, check if it's a closure variable.
  if (!callee) {
    auto local_it = locals.find(name);
    if (local_it != locals.end()) {
      auto *alloca = local_it->second;
      // Check if this local holds a closure fat pointer (either directly
      // as closure_fat_ptr_type, or as a ptr to one for function parameters).
      auto callee_sem = semantic_type(*node.callee);
      bool is_func_typed = callee_sem && callee_sem->kind == TypeKind::Func;
      if (alloca->getAllocatedType() == closure_fat_ptr_type || is_func_typed) {
        // Determine the closure struct pointer.
        // If the alloca directly holds a closure struct, use it.
        // If the alloca holds a ptr (function parameter), load first.
        auto *ptr_type = llvm::PointerType::getUnqual(context);
        llvm::Value *closure_ptr = alloca;
        if (alloca->getAllocatedType() != closure_fat_ptr_type) {
          // Load the pointer to the closure struct.
          closure_ptr = builder.CreateLoad(ptr_type, alloca, "cl.load");
        }

        // Load fn_ptr and env_ptr from the closure.
        auto *fn_gep = builder.CreateStructGEP(closure_fat_ptr_type,
                                                closure_ptr, 0, "cl.fn.ptr");
        auto *fn_ptr = builder.CreateLoad(ptr_type, fn_gep, "cl.fn");
        auto *env_gep = builder.CreateStructGEP(closure_fat_ptr_type,
                                                  closure_ptr, 1, "cl.env.ptr");
        auto *env_ptr = builder.CreateLoad(ptr_type, env_gep, "cl.env");

        // Build the indirect call: fn(env, args...)
        std::vector<llvm::Value *> args;
        args.push_back(env_ptr);
        for (auto &arg_node : node.args) {
          auto *val = emit_expr(*arg_node);
          if (val)
            args.push_back(val);
        }

        // Build the function type from the semantic type.
        auto callee_sem = semantic_type(*node.callee);
        std::vector<llvm::Type *> param_types;
        param_types.push_back(ptr_type); // env
        llvm::Type *ret_ll = void_ll_type;

        if (callee_sem && callee_sem->kind == TypeKind::Func) {
          auto &fi = std::get<FuncTypeInfo>(callee_sem->detail);
          for (auto &pt : fi.params)
            param_types.push_back(llvm_type(pt));
          if (!fi.returns.empty())
            ret_ll = llvm_type(fi.returns[0]);
        } else {
          // Fallback: infer from args.
          for (size_t i = 1; i < args.size(); ++i)
            param_types.push_back(args[i]->getType());
        }

        auto *fn_type = llvm::FunctionType::get(ret_ll, param_types, false);
        if (ret_ll->isVoidTy()) {
          builder.CreateCall(fn_type, fn_ptr, args);
          return nullptr;
        }
        return builder.CreateCall(fn_type, fn_ptr, args, "cl.call");
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
  auto cap_it = analyzer.node_captures.find(&parent);
  if (cap_it != analyzer.node_captures.end())
    captures = cap_it->second;

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
  auto cap_it = analyzer.spawn_captures.find(&parent);
  if (cap_it != analyzer.spawn_captures.end())
    captures = cap_it->second;

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
    // Resolve the channel element type.
    auto &type_param_node = *node.generic->type_params[0];
    auto chan_elem_sem = analyzer.resolve_type(type_param_node);
    auto *chan_elem_ll = llvm_type(chan_elem_sem);
    channel_elem_size = static_cast<int64_t>(
        module->getDataLayout().getTypeAllocSize(chan_elem_ll));

    auto *ch = builder.CreateCall(
        module->getFunction("mc_channel_new"),
        {llvm::ConstantInt::get(i64_type, channel_elem_size),
         llvm::ConstantInt::get(i64_type, 0)}, // 0 = default capacity
        "channel");
    channel_ptr = ch;
  }

  // ── Spawn the actor ────────────────────────────────────────────────
  auto *actor = builder.CreateCall(
      module->getFunction("mc_executor_spawn"),
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

  return nullptr;
}

} // namespace mc

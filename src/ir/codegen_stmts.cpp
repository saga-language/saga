// Copyright 2026 Rob Thornton
// SPDX-License-Identifier: MIT

#include "ir/codegen.hpp"

#include <llvm/IR/Constants.h>
#include <llvm/IR/Verifier.h>

namespace saga {


// ===========================================================================
// Function type building
// ===========================================================================

llvm::Type *CodeGen::resolve_type_node(const Node &type_node) {
  if (auto *ident = std::get_if<IdentifierNode>(&type_node.data)) {
    std::string tname(ident->name);
    // Keys are qualified (pkg__Name), so fall back via key_for when origin is unknown.
    auto sit = struct_types.find(key_for("", tname));
    if (sit != struct_types.end())
      return sit->second; // struct value type
    if (enum_types.count(key_for("", tname)))
      return i64_type; // enum as i64 tag
    if (iface_vtable_types.count(key_for("", tname)))
      return llvm::PointerType::getUnqual(context); // interface as ptr to fat ptr
  }
  // Cross-package types appear as SelectorNode (`pkg.Type`).  Look the
  // selector up via the package scope so we don't depend on current_scope
  // state at codegen time.
  if (auto *sel = std::get_if<SelectorNode>(&type_node.data)) {
    if (auto *pkg_ident = std::get_if<IdentifierNode>(&sel->object->data)) {
      std::optional<Symbol> sym;
      if (analyzer.package_scope_) {
        auto it = analyzer.package_scope_->symbols.find(
            std::string(pkg_ident->name));
        if (it != analyzer.package_scope_->symbols.end())
          sym = it->second;
      }
      if (!sym)
        sym = analyzer.lookup(std::string(pkg_ident->name));
      if (sym && sym->type && sym->type->kind == TypeKind::Module) {
        auto &mod = std::get<ModuleTypeInfo>(sym->type->detail);
        std::string field_name(sel->field.name);
        for (auto &exp : mod.exports) {
          if (exp.name == field_name && exp.type)
            return llvm_type(exp.type);
        }
      }
    }
  }
  // Fall back to analyzer resolve (works for builtins).
  auto sem_type = analyzer.resolve_type(type_node);
  return llvm_type(sem_type);
}

llvm::FunctionType *CodeGen::build_func_type(const FuncDeclNode &fn) {
  bool is_main = (fn.name.name == "Main");
  std::string link_name = is_main ? "main" : mangle(std::string(fn.name.name));

  // Determine semantic-level return.  Struct returns are lowered to sret:
  // a hidden first parameter `ptr sret(%T)`, the LLVM return type is void.
  llvm::Type *ret_type = void_ll_type;
  llvm::Type *sret_struct_ty = nullptr;
  if (is_main) {
    ret_type = llvm::Type::getInt32Ty(context);
  } else if (fn.signature.returns.size() == 1) {
    auto *r_ll = resolve_type_node(*fn.signature.returns[0]);
    if (r_ll && r_ll->isStructTy()) {
      sret_struct_ty = r_ll;
      ret_type = void_ll_type;
    } else {
      ret_type = r_ll;
    }
  } else if (fn.signature.returns.size() > 1) {
    // Multiple return values → pack into an LLVM struct.
    std::vector<llvm::Type *> ret_fields;
    for (auto &r : fn.signature.returns)
      ret_fields.push_back(resolve_type_node(*r));
    auto *st = llvm::StructType::create(context, ret_fields,
                                        "saga.ret." + link_name);
    multi_return_types[link_name] = st;
    multi_return_counts[link_name] = fn.signature.returns.size();
    ret_type = st;
  }

  // Parameter types.  Structs are lowered to `ptr` for byval.
  std::vector<llvm::Type *> param_types;
  if (sret_struct_ty)
    param_types.push_back(llvm::PointerType::getUnqual(context));
  if (!is_main) {
    for (auto &param : fn.signature.params) {
      auto *ll_type = resolve_type_node(*param.type);
      // Variadic params are arrays at the LLVM level (ptr to saga_runtime_array).
      if (param.is_variadic)
        ll_type = llvm::PointerType::getUnqual(context);
      // Struct params: byval lowering.  At the LLVM level the param slot
      // is `ptr`; the byval(%T) attribute is attached separately.
      else if (ll_type && ll_type->isStructTy())
        ll_type = llvm::PointerType::getUnqual(context);
      for (size_t i = 0; i < param.names.identifiers.size(); ++i)
        param_types.push_back(ll_type);
    }
  }

  return llvm::FunctionType::get(ret_type, param_types, /*isVarArg=*/false);
}

void CodeGen::apply_func_abi_attrs(llvm::Function *func,
                                    const FuncDeclNode &fn) {
  if (fn.name.name == "Main")
    return;
  unsigned idx = 0;
  // Sret return
  if (fn.signature.returns.size() == 1) {
    auto *r_ll = resolve_type_node(*fn.signature.returns[0]);
    if (r_ll && r_ll->isStructTy()) {
      llvm::AttrBuilder ab(context);
      ab.addStructRetAttr(r_ll);
      ab.addAlignmentAttr(
          module->getDataLayout().getABITypeAlign(r_ll));
      func->addParamAttrs(idx, ab);
      ++idx;
    }
  }
  // Byval struct params
  for (auto &param : fn.signature.params) {
    auto *p_ll = resolve_type_node(*param.type);
    bool byval = p_ll && p_ll->isStructTy() && !param.is_variadic;
    for (size_t i = 0; i < param.names.identifiers.size(); ++i) {
      if (byval) {
        llvm::AttrBuilder ab(context);
        ab.addByValAttr(p_ll);
        ab.addAlignmentAttr(
            module->getDataLayout().getABITypeAlign(p_ll));
        func->addParamAttrs(idx, ab);
      }
      ++idx;
    }
  }
}

// ===========================================================================
// Function body emission
// ===========================================================================

void CodeGen::emit_func_decl(const FuncDeclNode &fn) {
  if (fn.generic) {
    if (!fn.receiver)
      return;
    auto &rt = fn.receiver->type->data;
    bool is_generic_recv = std::get_if<ArrayTypeNode>(&rt) ||
                           std::get_if<MapTypeNode>(&rt);
    if (!is_generic_recv)
      return;
  } else if (fn.receiver) {
    // Receiver method bodies are emitted by their own paths.
    return;
  }

  std::string name(fn.name.name);
  bool is_main = (name == "Main");
  std::string link_name = is_main ? "main" : mangle(name);

  auto *func = module->getFunction(link_name);
  if (!func)
    return; // Should have been forward-declared.

  // Build the LLVM parameter types from the AST annotations (same logic
  // as build_func_type uses).  Specialised emission computes them from
  // bindings instead.
  std::vector<llvm::Type *> param_ll;
  if (!is_main) {
    for (auto &param : fn.signature.params) {
      auto *ll_type = resolve_type_node(*param.type);
      if (param.is_variadic)
        ll_type = llvm::PointerType::getUnqual(context);
      for (size_t i = 0; i < param.names.identifiers.size(); ++i)
        param_ll.push_back(ll_type);
    }
  }

  emit_function_body_inner(fn, func, param_ll, is_main);
}

void CodeGen::emit_function_body_inner(
    const FuncDeclNode &fn, llvm::Function *func,
    const std::vector<llvm::Type *> &param_ll, bool is_main) {
  auto *entry = llvm::BasicBlock::Create(context, "entry", func);
  builder.SetInsertPoint(entry);

  // Reset per-function state.
  locals.clear();
  managed_locals.clear();
  current_func_is_main = is_main;

  // Main begins with package initialisation: every imported package's
  // `<pkg>__init__` (topo order, deps first) plus this package's own
  // init.  Runs before saga_executor_init so init code never observes
  // a live executor (TD4).
  if (is_main)
    emit_init_calls();

  // If this is Main and we have spawn expressions, init the executor.
  if (is_main && has_spawn) {
    builder.CreateCall(module->getFunction("saga_executor_init"),
                       {llvm::ConstantInt::get(i64_type, 0)});
  }

  // Skip the hidden sret arg if present.
  size_t arg_idx = 0;
  bool has_sret = false;
  if (!is_main && fn.signature.returns.size() == 1) {
    auto *r_ll = resolve_type_node(*fn.signature.returns[0]);
    if (r_ll && r_ll->isStructTy()) {
      has_sret = true;
      ++arg_idx; // arg 0 is the sret pointer
    }
  }

  // Create allocas for parameters and store the incoming argument values.
  // param_ll has one entry per flattened parameter name so variadic /
  // multi-name params are already expanded.
  //
  // Array params are owned by the callee — emit_call_expr clones array
  // args at the call boundary (spec value semantics, docs/language.md:51).
  // Tracking them as managed locals releases the clone at function exit.
  size_t ll_idx = 0;
  for (auto &param : fn.signature.params) {
    auto param_sem = semantic_type(*param.type);
    for (auto &ident : param.names.identifiers) {
      auto *ll_type = ll_idx < param_ll.size()
                          ? param_ll[ll_idx]
                          : llvm::PointerType::getUnqual(context);
      std::string pname(ident.name);
      auto *arg = func->getArg(arg_idx++);
      auto *alloca = create_entry_alloca(func, pname, ll_type);
      if (ll_type && ll_type->isStructTy()) {
        // Byval struct param: arg is a `ptr` to a stable caller-provided
        // copy.  Memcpy its bytes into the local alloca so subsequent
        // mutations stay local to this frame.
        auto sz = module->getDataLayout().getTypeAllocSize(ll_type);
        auto al = module->getDataLayout().getABITypeAlign(ll_type);
        builder.CreateMemCpy(alloca, al, arg, al, sz);
      } else {
        builder.CreateStore(arg, alloca);
      }
      locals[pname] = alloca;
      if (param_sem && param_sem->kind == TypeKind::Array)
        track_managed(pname, param_sem);
      ++ll_idx;
    }
  }
  (void)has_sret;

  // Emit body.
  auto &block = std::get<BlockNode>(fn.body->data);
  auto *tail_val = emit_block(block);

  // If the block didn't already terminate, release locals and return.
  if (!builder.GetInsertBlock()->getTerminator()) {
    emit_release_locals();
    auto *ret_type = func->getReturnType();
    if (is_main) {
      if (has_spawn)
        builder.CreateCall(module->getFunction("saga_executor_shutdown"), {});
      builder.CreateRet(
          llvm::ConstantInt::get(llvm::Type::getInt32Ty(context), 0));
    } else if (has_sret) {
      // Struct return via sret.  tail_val is either a pointer to a struct
      // alloca (struct literal, identifier, byval param) or a struct SSA
      // value (loaded from an alloca).  Copy into the sret slot.
      auto *sret_arg = func->getArg(0);
      llvm::Type *struct_ty =
          resolve_type_node(*fn.signature.returns[0]);
      if (tail_val && struct_ty && struct_ty->isStructTy()) {
        if (tail_val->getType()->isPointerTy()) {
          auto sz = module->getDataLayout().getTypeAllocSize(struct_ty);
          auto al = module->getDataLayout().getABITypeAlign(struct_ty);
          builder.CreateMemCpy(sret_arg, al, tail_val, al, sz);
        } else if (tail_val->getType() == struct_ty) {
          builder.CreateStore(tail_val, sret_arg);
        }
      }
      builder.CreateRetVoid();
    } else if (ret_type->isVoidTy()) {
      builder.CreateRetVoid();
    } else if (tail_val && tail_val->getType() == ret_type) {
      builder.CreateRet(tail_val);
    } else if (tail_val && ret_type->isIntegerTy() &&
               tail_val->getType()->isIntegerTy() &&
               tail_val->getType() != ret_type) {
      // Integer width mismatch (e.g. runtime returns i64, function returns i1).
      unsigned src_bits = tail_val->getType()->getIntegerBitWidth();
      unsigned dst_bits = ret_type->getIntegerBitWidth();
      llvm::Value *conv;
      if (src_bits > dst_bits)
        conv = builder.CreateTrunc(tail_val, ret_type, "ret.trunc");
      else
        conv = builder.CreateZExt(tail_val, ret_type, "ret.zext");
      builder.CreateRet(conv);
    } else if (tail_val && ret_type->isStructTy() &&
               tail_val->getType()->isPointerTy()) {
      // Union tail: pointer to union struct — load and return.
      auto *st = llvm::cast<llvm::StructType>(ret_type);
      if (st->getNumElements() == 2 &&
          st->getElementType(0)->isIntegerTy(8) &&
          st->getElementType(1)->isArrayTy()) {
        if (auto *ai = llvm::dyn_cast<llvm::AllocaInst>(tail_val)) {
          if (ai->getAllocatedType() == ret_type) {
            auto *loaded = builder.CreateLoad(ret_type, tail_val, "ret.union");
            builder.CreateRet(loaded);
          } else {
            builder.CreateRet(llvm::Constant::getNullValue(ret_type));
          }
        } else {
          builder.CreateRet(llvm::Constant::getNullValue(ret_type));
        }
      } else {
        builder.CreateRet(llvm::Constant::getNullValue(ret_type));
      }
    } else {
      builder.CreateRet(llvm::Constant::getNullValue(ret_type));
    }
  }

  llvm::verifyFunction(*func);
}

// ===========================================================================
// Block / statement emission
// ===========================================================================

llvm::Value *CodeGen::emit_block(const BlockNode &block) {
  llvm::Value *last = nullptr;
  for (auto &stmt : block.stmts) {
    // If we already have a terminator (e.g. from a return), stop.
    if (builder.GetInsertBlock()->getTerminator())
      break;
    last = emit_expr(*stmt);
  }
  return last;
}

void CodeGen::emit_stmt(const Node &node) {
  std::visit(
      overloaded{
          [&](const VarDeclNode &n) { emit_var_decl(n); },
          [&](const DeclAssignNode &n) { emit_decl_assign(n); },
          [&](const AssignNode &n) { emit_assign(n); },
          [&](const ReturnNode &n) { emit_return(n); },
          [&](const IncrementNode &n) { emit_increment(n); },
          [&](const DecrementNode &n) { emit_decrement(n); },
          [&](const auto &) {
            // Everything else is an expression evaluated for side effects.
            emit_expr(node);
          },
      },
      node.data);
}

// ===========================================================================
// Statement emitters
// ===========================================================================

void CodeGen::emit_var_decl(const VarDeclNode &node) {
  std::string name(node.name.name);
  auto *func = builder.GetInsertBlock()->getParent();

  // Determine the LLVM type from the semantic type annotation or
  // the initializer's type.
  llvm::Type *var_type = i64_type; // default to Int
  if (node.type) {
    // Check node_types first (recorded during analysis), then fallback.
    if (auto recorded = semantic_type(**node.type)) {
      var_type = llvm_type(recorded);
    } else {
      auto sem_type = analyzer.resolve_type(**node.type);
      var_type = llvm_type(sem_type);
    }
  } else if (node.init) {
    // Infer from the init expression's semantic type.
    if (auto recorded = semantic_type(**node.init))
      var_type = llvm_type(recorded);
  }

  // Determine semantic type for refcount tracking and interface boxing.
  TypePtr sem_type_ptr = nullptr;
  if (node.type) {
    // Look up from the node_types map first (recorded during analysis).
    sem_type_ptr = semantic_type(**node.type);
    if (!sem_type_ptr) {
      // Fall back to resolve_type (works for builtins).
      sem_type_ptr = analyzer.resolve_type(**node.type);
    }
  } else if (node.init) {
    sem_type_ptr = semantic_type(**node.init);
  }

  if (node.init) {
    auto *val = emit_expr(**node.init);
    // Interface boxing: declared type is interface, init is a concrete struct.
    if (sem_type_ptr && sem_type_ptr->kind == TypeKind::Interface) {
      auto init_sem = semantic_type(**node.init);
      if (init_sem && init_sem->kind == TypeKind::Struct) {
        // We need the struct pointer, not the loaded value.
        // Check if the init expression is an identifier referencing
        // a local struct alloca.
        llvm::Value *struct_ptr = val;
        if (auto *id = std::get_if<IdentifierNode>(&(*node.init)->data)) {
          auto local_it = locals.find(std::string(id->name));
          if (local_it != locals.end())
            struct_ptr = local_it->second; // The alloca pointer.
        }
        if (struct_ptr) {
          auto *boxed = emit_interface_box(struct_ptr, init_sem, sem_type_ptr);
          if (boxed) {
            if (auto *ba = llvm::dyn_cast<llvm::AllocaInst>(boxed)) {
              ba->setName(name);
              locals[name] = ba;
            }
            return;
          }
        }
      }
    }

    // Union boxing: declared type is a union, init is a concrete type.
    if (val && sem_type_ptr && sem_type_ptr->kind == TypeKind::Union) {
      auto init_sem = semantic_type(**node.init);
      if (init_sem && init_sem->kind != TypeKind::Union) {
        auto *wrapped = emit_union_wrap(val, init_sem, sem_type_ptr);
        if (wrapped && llvm::isa<llvm::AllocaInst>(wrapped)) {
          auto *alloca = llvm::cast<llvm::AllocaInst>(wrapped);
          alloca->setName(name);
          locals[name] = alloca;
          track_managed(name, sem_type_ptr);
          return;
        }
      }
      // If the init already produces a union (e.g. from a call), alias it.
      if (init_sem && init_sem->kind == TypeKind::Union &&
          val && llvm::isa<llvm::AllocaInst>(val)) {
        auto *alloca = llvm::cast<llvm::AllocaInst>(val);
        alloca->setName(name);
        locals[name] = alloca;
        track_managed(name, sem_type_ptr);
        return;
      }
    }

    // Struct init: copy into a fresh local alloca to preserve value
    // semantics.  RHS may be either a pointer (alloca / sret slot) or a
    // struct SSA value.
    {
      auto sem = semantic_type(**node.init);
      if (val && sem && sem->kind == TypeKind::Struct) {
        auto &sinfo = std::get<StructTypeInfo>(sem->detail);
        std::string skey = struct_cache_key(sinfo);
        auto st_it = struct_types.find(skey);
        if (st_it != struct_types.end()) {
          auto *st_type = st_it->second;
          auto *alloca = create_entry_alloca(func, name, st_type);
          if (val->getType()->isPointerTy()) {
            auto sz = module->getDataLayout().getTypeAllocSize(st_type);
            auto al = module->getDataLayout().getABITypeAlign(st_type);
            builder.CreateMemCpy(alloca, al, val, al, sz);
          } else if (val->getType() == st_type) {
            builder.CreateStore(val, alloca);
          }
          locals[name] = alloca;
          track_managed(name, sem);
          return;
        }
      }
    }

    // If the init produces a union alloca, alias it.
    if (val && llvm::isa<llvm::AllocaInst>(val)) {
      auto init_sem = semantic_type(**node.init);
      if (init_sem && init_sem->kind == TypeKind::Union) {
        auto *alloca = llvm::cast<llvm::AllocaInst>(val);
        alloca->setName(name);
        locals[name] = alloca;
        track_managed(name, sem_type_ptr);
        return;
      }
    }

    // If the init is a closure alloca, alias it directly.
    if (val && llvm::isa<llvm::AllocaInst>(val)) {
      auto *alloca = llvm::cast<llvm::AllocaInst>(val);
      if (alloca->getAllocatedType() == closure_fat_ptr_type) {
        alloca->setName(name);
        locals[name] = alloca;
        return;
      }
    }

    auto *alloca = create_entry_alloca(func, name, var_type);
    locals[name] = alloca;
    if (val)
      builder.CreateStore(val, alloca);
  } else {
    // Zero-initialize with proper language zero values.
    // The language specifies: Int=0, Float=0.0, Bool=false, String="",
    // [T]=[], {K:V}={}, Struct=all-fields-zero.
    if (sem_type_ptr && sem_type_ptr->kind == TypeKind::String) {
      // String zero value: empty string ""
      auto *empty_str = make_string_constant("");
      auto *alloca = create_entry_alloca(func, name, var_type);
      locals[name] = alloca;
      builder.CreateStore(empty_str, alloca);
    } else if (sem_type_ptr && sem_type_ptr->kind == TypeKind::Array) {
      // Array zero value: empty array []
      auto &arr_info = std::get<ArrayTypeInfo>(sem_type_ptr->detail);
      int64_t elem_size = 8; // default
      if (arr_info.element) {
        auto *elem_ll = llvm_type(arr_info.element);
        if (elem_ll->isIntegerTy(1))
          elem_size = 1;
        else if (elem_ll->isDoubleTy() || elem_ll->isIntegerTy(64) ||
                 elem_ll->isPointerTy())
          elem_size = 8;
      }
      auto *new_fn = module->getFunction("saga_array_new");
      auto *arr = builder.CreateCall(
          new_fn,
          {llvm::ConstantInt::get(i64_type, elem_size),
           llvm::ConstantInt::get(i64_type, 4)},
          "arr");
      auto *alloca = create_entry_alloca(func, name, var_type);
      locals[name] = alloca;
      builder.CreateStore(arr, alloca);
    } else if (sem_type_ptr && sem_type_ptr->kind == TypeKind::Map) {
      // Map zero value: empty map {}
      auto &map_info = std::get<MapTypeInfo>(sem_type_ptr->detail);
      int64_t key_size = 8;
      int64_t val_size = 8;
      if (map_info.key) {
        auto *key_ll = llvm_type(map_info.key);
        if (key_ll->isStructTy())
          key_size = module->getDataLayout().getTypeAllocSize(key_ll);
        else if (key_ll->isIntegerTy(1))
          key_size = 1;
      }
      if (map_info.value) {
        auto *val_ll = llvm_type(map_info.value);
        if (val_ll->isStructTy())
          val_size = module->getDataLayout().getTypeAllocSize(val_ll);
        else if (val_ll->isIntegerTy(1))
          val_size = 1;
      }
      int64_t key_kind_tag =
          static_cast<int64_t>(CodeGen::key_kind_for(map_info.key));
      llvm::Constant *ops_ptr = get_or_emit_key_ops(map_info.key);
      auto *new_fn = module->getFunction("saga_map_new");
      auto *map = builder.CreateCall(
          new_fn,
          {llvm::ConstantInt::get(i64_type, key_size),
           llvm::ConstantInt::get(i64_type, val_size),
           llvm::ConstantInt::get(i64_type, key_kind_tag),
           ops_ptr},
          "map");
      auto *alloca = create_entry_alloca(func, name, var_type);
      locals[name] = alloca;
      builder.CreateStore(map, alloca);
    } else if (sem_type_ptr && sem_type_ptr->kind == TypeKind::Struct) {
      // Struct zero value: allocate struct, zero-initialize all fields.
      auto &info = std::get<StructTypeInfo>(sem_type_ptr->detail);
      std::string skey = struct_cache_key(info);
      auto st_it = struct_types.find(skey);
      if (st_it != struct_types.end()) {
        auto *st_type = st_it->second;
        auto *alloca = create_entry_alloca(func, name, st_type);
        locals[name] = alloca;
        builder.CreateStore(llvm::Constant::getNullValue(st_type), alloca);
      } else {
        auto *alloca = create_entry_alloca(func, name, var_type);
        locals[name] = alloca;
        builder.CreateStore(llvm::Constant::getNullValue(var_type), alloca);
      }
    } else {
      // Scalar types (Int, Float, Bool, Enum, etc.): getNullValue is correct.
      auto *alloca = create_entry_alloca(func, name, var_type);
      locals[name] = alloca;
      builder.CreateStore(llvm::Constant::getNullValue(var_type), alloca);
    }
  }

  // Track for release at scope exit.
  track_managed(name, sem_type_ptr);
}

void CodeGen::emit_decl_assign(const DeclAssignNode &node) {
  auto *val = emit_expr(*node.value);
  auto *func = builder.GetInsertBlock()->getParent();
  auto val_sem = semantic_type(*node.value);

  // ── Multi-return unpacking ───────────────────────────────────────────
  // If there are multiple targets and the RHS is a multi-return struct,
  // extract each field into its own local variable.
  if (node.targets.identifiers.size() > 1 && val &&
      val->getType()->isStructTy()) {
    // Check if this is a known multi-return struct.
    std::string callee_name;
    if (auto *call = std::get_if<CallExprNode>(&node.value->data)) {
      if (auto *ident = std::get_if<IdentifierNode>(&call->callee->data))
        callee_name = std::string(ident->name);
    }
    std::string link_name = (callee_name == "Main") ? "main" : mangle(callee_name);
    auto mr_it = multi_return_types.find(link_name);

    if (!callee_name.empty() && mr_it != multi_return_types.end()) {
      auto *ret_st = mr_it->second;
      size_t count = std::min(node.targets.identifiers.size(),
                              (size_t)ret_st->getNumElements());

      // Resolve per-element semantic types from the callee's signature.
      // Look up the FuncDeclNode via the analyzer to get return types.
      std::vector<TypePtr> elem_sem_types;
      auto fn_sem = val_sem;
      if (fn_sem && fn_sem->kind == TypeKind::Func) {
        auto &fi = std::get<FuncTypeInfo>(fn_sem->detail);
        elem_sem_types = fi.returns;
      }

      for (size_t i = 0; i < count; ++i) {
        std::string name(node.targets.identifiers[i].name);
        auto *elem_val = builder.CreateExtractValue(val, i,
            name + ".unpack");
        auto *alloca = create_entry_alloca(func, name, elem_val->getType());
        locals[name] = alloca;
        builder.CreateStore(elem_val, alloca);

        // Track managed types.
        if (i < elem_sem_types.size())
          track_managed(name, elem_sem_types[i]);
      }
      return;
    }
  }

  // ── Single value assignment ──────────────────────────────────────────
  for (auto &ident : node.targets.identifiers) {
    std::string name(ident.name);

    // Struct values: copy into a fresh local alloca to preserve value
    // semantics under D1 ABI. Source may be a pointer (alloca/sret slot)
    // or an SSA struct value.
    {
      auto sem = semantic_type(*node.value);
      if (val && sem && sem->kind == TypeKind::Struct) {
        auto &sinfo = std::get<StructTypeInfo>(sem->detail);
        std::string skey = struct_cache_key(sinfo);
        auto st_it = struct_types.find(skey);
        if (st_it != struct_types.end()) {
          auto *st_type = st_it->second;
          auto *alloca = create_entry_alloca(func, name, st_type);
          if (val->getType()->isPointerTy()) {
            auto sz = module->getDataLayout().getTypeAllocSize(st_type);
            auto al = module->getDataLayout().getABITypeAlign(st_type);
            builder.CreateMemCpy(alloca, al, val, al, sz);
          } else if (val->getType() == st_type) {
            builder.CreateStore(val, alloca);
          }
          locals[name] = alloca;
          track_managed(name, sem);
          continue;
        }
      }
    }

    // Union and closure alloca: alias directly.
    if (val && llvm::isa<llvm::AllocaInst>(val)) {
      auto *alloca = llvm::cast<llvm::AllocaInst>(val);
      auto sem = semantic_type(*node.value);
      if (sem && sem->kind == TypeKind::Union) {
        alloca->setName(name);
        locals[name] = alloca;
        track_managed(name, sem);
        continue;
      }
      if (alloca->getAllocatedType() == closure_fat_ptr_type) {
        alloca->setName(name);
        locals[name] = alloca;
        continue;
      }
    }

    // Union RHS via pointer (e.g. `result := for ... { break v }`):
    // emit_for_expr returns a ptr to a union struct.  Allocate a
    // struct-typed local and memcpy through.
    if (val && val_sem && val_sem->kind == TypeKind::Union &&
        val->getType()->isPointerTy()) {
      auto *union_st = get_union_llvm_type(val_sem);
      if (union_st) {
        auto *alloca = create_entry_alloca(func, name, union_st);
        auto sz = module->getDataLayout().getTypeAllocSize(union_st);
        auto al = module->getDataLayout().getABITypeAlign(union_st);
        builder.CreateMemCpy(alloca, al, val, al, sz);
        locals[name] = alloca;
        track_managed(name, val_sem);
        continue;
      }
    }

    llvm::Type *var_type = val ? val->getType() : i64_type;
    auto *alloca = create_entry_alloca(func, name, var_type);
    locals[name] = alloca;
    if (val)
      builder.CreateStore(val, alloca);

    // Track managed types for release at scope exit.
    track_managed(name, val_sem);

    // If a pending channel alloca exists from a spawn expression,
    // create a companion local "<name>.channel" for for-range iteration.
    if (pending_channel_alloca_) {
      std::string ch_name = name + ".channel";
      locals[ch_name] = pending_channel_alloca_;
      pending_channel_alloca_ = nullptr;
    }
  }
}

void CodeGen::emit_assign(const AssignNode &node) {
  for (size_t i = 0; i < node.targets.size() && i < node.values.size(); ++i) {
    auto *rhs = emit_expr(*node.values[i]);
    if (!rhs)
      continue;

    // Target can be an identifier, selector, or index expression.
    if (auto *idx_expr = std::get_if<IndexExprNode>(&node.targets[i]->data)) {
      // Index assignment: obj[key] = rhs
      auto *obj = emit_expr(*idx_expr->object);
      auto *key = emit_expr(*idx_expr->index);
      if (!obj || !key)
        continue;

      auto obj_sem = semantic_type(*idx_expr->object);
      if (obj_sem && obj_sem->kind == TypeKind::Map) {
        auto *func = builder.GetInsertBlock()->getParent();
        auto *key_tmp = create_entry_alloca(func, "map.asgn.key", key->getType());
        builder.CreateStore(key, key_tmp);
        auto *val_tmp = create_entry_alloca(func, "map.asgn.val", rhs->getType());
        builder.CreateStore(rhs, val_tmp);

        auto *set_fn = module->getFunction("saga_map_set");
        builder.CreateCall(set_fn, {obj, key_tmp, val_tmp});
      } else if (obj_sem && obj_sem->kind == TypeKind::Array) {
        // Array index assignment: arr[idx] = rhs
        // TODO: implement saga_runtime_array_set when available
      }
      continue;
    }

    if (auto *sel = std::get_if<SelectorNode>(&node.targets[i]->data)) {
      // Field assignment: obj.field = rhs
      std::string field_name(sel->field.name);
      if (auto *ident = std::get_if<IdentifierNode>(&sel->object->data)) {
        auto local_it = locals.find(std::string(ident->name));
        if (local_it != locals.end()) {
          auto sem = semantic_type(*sel->object);
          if (sem && sem->kind == TypeKind::Struct) {
            auto [gep, ftype] =
                struct_field_gep(local_it->second, sem, field_name);
            if (gep) {
              if (node.op == Token::Kind::Assignment) {
                builder.CreateStore(rhs, gep);
              } else {
                auto *cur = builder.CreateLoad(ftype, gep);
                llvm::Value *result = nullptr;
                using K = Token::Kind;
                switch (node.op) {
                case K::AddAssignment: result = builder.CreateAdd(cur, rhs, "add"); break;
                case K::SubAssignment: result = builder.CreateSub(cur, rhs, "sub"); break;
                case K::MulAssignment: result = builder.CreateMul(cur, rhs, "mul"); break;
                case K::DivAssignment: result = builder.CreateSDiv(cur, rhs, "div"); break;
                default: result = rhs; break;
                }
                builder.CreateStore(result, gep);
              }
            }
          }
        }
      }
      continue;
    }

    auto *ident = std::get_if<IdentifierNode>(&node.targets[i]->data);
    if (!ident)
      continue;

    auto it = locals.find(std::string(ident->name));
    if (it == locals.end())
      continue;

    auto *alloca = it->second;

    auto target_sem = semantic_type(*node.targets[i]);

    using K = Token::Kind;
    if (node.op == K::Assignment) {
      // Release old value before overwriting if managed.
      if (target_sem && (target_sem->kind == TypeKind::String ||
                         target_sem->kind == TypeKind::Array ||
                         target_sem->kind == TypeKind::Map)) {
        auto *old = builder.CreateLoad(alloca->getAllocatedType(), alloca);
        emit_release(old, target_sem);
      }
      builder.CreateStore(rhs, alloca);
    } else {
      // Compound assignment: load current, apply op, store.
      auto *cur = builder.CreateLoad(alloca->getAllocatedType(), alloca);
      llvm::Value *result = nullptr;

      // Check if this is a string compound assignment.
      bool is_str = target_sem && target_sem->kind == TypeKind::String;

      if (is_str && node.op == K::AddAssignment) {
        auto *concat_fn = module->getFunction("saga_string_concat");
        result = builder.CreateCall(concat_fn, {cur, rhs}, "concat");
        // Release the old string since concat created a new one.
        emit_release(cur, target_sem);
      } else {
        switch (node.op) {
        case K::AddAssignment: result = builder.CreateAdd(cur, rhs, "add"); break;
        case K::SubAssignment: result = builder.CreateSub(cur, rhs, "sub"); break;
        case K::MulAssignment: result = builder.CreateMul(cur, rhs, "mul"); break;
        case K::DivAssignment: result = builder.CreateSDiv(cur, rhs, "div"); break;
        default: result = rhs; break;
        }
      }
      builder.CreateStore(result, alloca);
    }
  }
}

void CodeGen::emit_return(const ReturnNode &node) {
  if (current_func_is_main) {
    emit_release_locals();
    if (has_spawn)
      builder.CreateCall(module->getFunction("saga_executor_shutdown"), {});
    if (node.values.empty()) {
      builder.CreateRet(
          llvm::ConstantInt::get(llvm::Type::getInt32Ty(context), 0));
    } else {
      auto *val = emit_expr(*node.values[0]);
      auto *i32_val = builder.CreateTrunc(val, llvm::Type::getInt32Ty(context),
                                          "main_ret");
      builder.CreateRet(i32_val);
    }
    return;
  }

  if (node.values.empty()) {
    emit_release_locals();
    builder.CreateRetVoid();
  } else if (node.values.size() == 1) {
    auto *val = emit_expr(*node.values[0]);
    auto *func = builder.GetInsertBlock()->getParent();
    auto *ret_type = func->getReturnType();

    // Sret return: copy struct value/alloca into the hidden first arg.
    if (ret_type->isVoidTy() && func->arg_size() > 0 &&
        func->getArg(0)->hasStructRetAttr()) {
      auto *sret_arg = func->getArg(0);
      auto *struct_ty = func->getParamStructRetType(0);
      if (val && struct_ty) {
        if (val->getType()->isPointerTy()) {
          auto sz = module->getDataLayout().getTypeAllocSize(struct_ty);
          auto al = module->getDataLayout().getABITypeAlign(struct_ty);
          builder.CreateMemCpy(sret_arg, al, val, al, sz);
        } else if (val->getType() == struct_ty) {
          builder.CreateStore(val, sret_arg);
        }
      }
      emit_release_locals();
      builder.CreateRetVoid();
      return;
    }

    // Handle union return types: wrap concrete values or load from alloca.
    if (val && ret_type->isStructTy() && val->getType()->isPointerTy()) {
      auto *st = llvm::cast<llvm::StructType>(ret_type);
      // Check if return type is a union struct: { i8, [N x i8] }
      if (st->getNumElements() == 2 &&
          st->getElementType(0)->isIntegerTy(8) &&
          st->getElementType(1)->isArrayTy()) {
        // val is a pointer to the union alloca — load the struct value.
        if (auto *ai = llvm::dyn_cast<llvm::AllocaInst>(val)) {
          if (ai->getAllocatedType() == ret_type) {
            val = builder.CreateLoad(ret_type, val, "ret.union");
          }
        }
      }
    }
    // If val is a concrete value but ret_type is a union struct, wrap it.
    if (val && ret_type->isStructTy() && !val->getType()->isStructTy()) {
      auto *st = llvm::cast<llvm::StructType>(ret_type);
      if (st->getNumElements() == 2 &&
          st->getElementType(0)->isIntegerTy(8) &&
          st->getElementType(1)->isArrayTy()) {
        // Need to find the semantic return type and value type.
        auto val_sem = semantic_type(*node.values[0]);
        // Look up the function's semantic return type from the scope.
        TypePtr ret_sem = nullptr;
        for (auto &[key, union_st] : union_llvm_types) {
          if (union_st == st) {
            // Reconstruct semantic type is complex; use the analyzer's
            // return types from the current scope instead.
            break;
          }
        }
        // Use the analyzer's scope to get return types.
        if (!ret_sem && !analyzer.current_scope->return_types.empty()) {
          ret_sem = analyzer.current_scope->return_types[0];
        }
        if (val_sem && ret_sem && ret_sem->kind == TypeKind::Union) {
          auto *wrapped = emit_union_wrap(val, val_sem, ret_sem);
          if (wrapped)
            val = builder.CreateLoad(ret_type, wrapped, "ret.union");
        }
      }
    }

    emit_release_locals();
    if (val)
      builder.CreateRet(val);
    else
      builder.CreateRetVoid();
  } else {
    // Multiple return values — pack into a struct.
    auto *func = builder.GetInsertBlock()->getParent();
    std::string link_name(func->getName());
    auto it = multi_return_types.find(link_name);
    if (it != multi_return_types.end()) {
      auto *ret_st = it->second;
      llvm::Value *agg = llvm::UndefValue::get(ret_st);
      for (size_t i = 0; i < node.values.size(); ++i) {
        auto *val = emit_expr(*node.values[i]);
        if (val)
          agg = builder.CreateInsertValue(agg, val, i, "pack." + std::to_string(i));
      }
      emit_release_locals();
      builder.CreateRet(agg);
    } else {
      emit_release_locals();
      builder.CreateRetVoid();
    }
  }
}

void CodeGen::emit_increment(const IncrementNode &node) {
  auto *ident = std::get_if<IdentifierNode>(&node.operand->data);
  if (!ident) return;
  auto it = locals.find(std::string(ident->name));
  if (it == locals.end()) return;

  auto *alloca = it->second;
  auto *cur = builder.CreateLoad(alloca->getAllocatedType(), alloca);
  auto *one = llvm::ConstantInt::get(i64_type, 1);
  auto *inc = builder.CreateAdd(cur, one, "inc");
  builder.CreateStore(inc, alloca);
}

void CodeGen::emit_decrement(const DecrementNode &node) {
  auto *ident = std::get_if<IdentifierNode>(&node.operand->data);
  if (!ident) return;
  auto it = locals.find(std::string(ident->name));
  if (it == locals.end()) return;

  auto *alloca = it->second;
  auto *cur = builder.CreateLoad(alloca->getAllocatedType(), alloca);
  auto *one = llvm::ConstantInt::get(i64_type, 1);
  auto *dec = builder.CreateSub(cur, one, "dec");
  builder.CreateStore(dec, alloca);
}


} // namespace saga

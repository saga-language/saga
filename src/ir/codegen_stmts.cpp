// Copyright 2026 Rob Thornton
// SPDX-License-Identifier: MIT

#include "ir/codegen.hpp"

#include <llvm/IR/Constants.h>
#include <llvm/IR/Verifier.h>

#include <unordered_set>

namespace saga {


// ===========================================================================
// Function type building
// ===========================================================================

// Resolve a type node the way the analyzer would have at declaration time:
// from the package scope, since analysis left current_scope elsewhere. A miss
// is a failed lookup rather than a program error — checking is over — so it
// must not reach the user.
TypePtr CodeGen::lookup_sem_type(const Node &type_node) {
  Analyzer::Silence quiet(analyzer);
  Analyzer::AtPackageScope scope(analyzer);
  return analyzer.resolve_type(type_node);
}

// Named and qualified types resolve straight out of the package scope; every
// other node shape — including a generic application like `Box<int>` — goes
// through the analyzer.
llvm::Type *CodeGen::resolve_type_node(const Node &type_node) {
  // Prefer the type the analyzer already resolved for this node (recorded with
  // the package scope active). Codegen's current_scope can't resolve a
  // package-local name like the `E` in `int | E`, which would otherwise fall
  // through to the Invalid sentinel and desync the sret union from the caller.
  if (auto rec = semantic_type(type_node))
    return llvm_type(rec);

  auto *ident = std::get_if<IdentifierNode>(&type_node.data);
  if (ident) {
    auto *ll = named_type_llvm(ident->name);
    if (ll)
      return ll;
  }
  auto *sel = std::get_if<SelectorNode>(&type_node.data);
  if (sel) {
    auto *ll = qualified_type_llvm(*sel);
    if (ll)
      return ll;
  }
  return llvm_type(lookup_sem_type(type_node));
}

std::optional<Symbol> CodeGen::package_symbol(std::string_view name) {
  std::string key(name);
  if (analyzer.package_scope_) {
    auto it = analyzer.package_scope_->symbols.find(key);
    if (it != analyzer.package_scope_->symbols.end())
      return it->second;
  }
  return analyzer.lookup(key);
}

llvm::Type *CodeGen::named_type_llvm(std::string_view name) {
  auto sym = package_symbol(name);
  if (sym && sym->kind == SymbolKind::Type && sym->type)
    return llvm_type(sym->type);
  return nullptr;
}

llvm::Type *CodeGen::qualified_type_llvm(const SelectorNode &sel) {
  auto *pkg = std::get_if<IdentifierNode>(&sel.object->data);
  if (!pkg)
    return nullptr;
  auto sym = package_symbol(pkg->name);
  if (!sym || !sym->type || sym->type->kind != TypeKind::Module)
    return nullptr;
  auto &mod = std::get<ModuleTypeInfo>(sym->type->detail);
  for (auto &exp : mod.exports) {
    if (exp.name == sel.field.name && exp.type)
      return llvm_type(exp.type);
  }
  return nullptr;
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
  } else if (fn.signature.return_type) {
    auto *r_ll = resolve_type_node(*fn.signature.return_type);
    if (r_ll && r_ll->isStructTy()) {
      sret_struct_ty = r_ll;
      ret_type = void_ll_type;
    } else {
      ret_type = r_ll;
    }
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

llvm::FunctionType *
CodeGen::build_extern_generic_func_type(const FuncDeclNode &fn) {
  std::unordered_set<std::string> generic_names;
  if (fn.generic) {
    for (auto &tp : fn.generic->type_params) {
      if (auto opt_name = type_param_name(*tp))
        generic_names.insert(std::string(*opt_name));
    }
  }

  auto *ptr_ty = llvm::PointerType::getUnqual(context);
  auto lower = [&](const Node &type_node) -> llvm::Type * {
    if (auto *id = std::get_if<IdentifierNode>(&type_node.data))
      if (generic_names.count(std::string(id->name)))
        return ptr_ty;
    return resolve_type_node(type_node);
  };

  llvm::Type *ret_type = void_ll_type;
  if (fn.signature.return_type)
    ret_type = lower(*fn.signature.return_type);

  std::vector<llvm::Type *> param_types;
  for (auto &param : fn.signature.params) {
    auto *ll_type = lower(*param.type);
    if (ll_type && ll_type->isStructTy())
      ll_type = ptr_ty;
    for (size_t i = 0; i < param.names.identifiers.size(); ++i)
      param_types.push_back(ll_type);
  }
  return llvm::FunctionType::get(ret_type, param_types, /*isVarArg=*/false);
}

void CodeGen::apply_func_abi_attrs(llvm::Function *func,
                                    const FuncDeclNode &fn) {
  if (fn.name.name == "Main")
    return;
  unsigned idx = 0;
  // Sret return
  if (fn.signature.return_type) {
    auto *r_ll = resolve_type_node(*fn.signature.return_type);
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
  if (fn.is_extern) {
    // Bodiless declaration — the link-time symbol is resolved externally.
    return;
  }
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
  std::string link_name = free_func_link_name(fn);

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

  // If this is Main and we have spawn expressions, init the executor.
  if (is_main && has_spawn) {
    builder.CreateCall(module->getFunction("saga_executor_init"),
                       {llvm::ConstantInt::get(i64_type, 0)});
  }

  // Skip the hidden sret arg if present.
  size_t arg_idx = 0;
  bool has_sret = false;
  if (!is_main && fn.signature.return_type) {
    auto *r_ll = resolve_type_node(*fn.signature.return_type);
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
    if (is_main) {
      if (has_spawn)
        builder.CreateCall(module->getFunction("saga_executor_shutdown"), {});
      builder.CreateRet(
          llvm::ConstantInt::get(llvm::Type::getInt32Ty(context), 0));
    } else {
      emit_tail_return(fn, func, tail_val, block, has_sret);
    }
  }

  llvm::verifyFunction(*func);
}

void CodeGen::emit_tail_return(const FuncDeclNode &fn, llvm::Function *func,
                               llvm::Value *tail_val, const BlockNode &block,
                               bool has_sret) {
  auto *ret_type = func->getReturnType();
  if (has_sret) {
    // Struct return via sret.  tail_val is either a pointer to a struct
    // alloca (struct literal, identifier, byval param, or an if/switch branch
    // merge) or a struct SSA value.  Copy into the sret slot.  For a union
    // return, a bare/error tail value is first wrapped into union memory.
    auto *sret_arg = func->getArg(0);
    llvm::Type *struct_ty = resolve_type_node(*fn.signature.return_type);
    llvm::Value *src = tail_val;
    if (auto union_sem = union_sem_for_llvm(struct_ty)) {
      TypePtr tail_sem = block.stmts.empty()
                             ? nullptr
                             : semantic_type(*block.stmts.back());
      src = as_union_ptr(tail_val, tail_sem, union_sem);
    }
    if (src && struct_ty && struct_ty->isStructTy() &&
        src->getType()->isPointerTy()) {
      auto sz = module->getDataLayout().getTypeAllocSize(struct_ty);
      auto al = module->getDataLayout().getABITypeAlign(struct_ty);
      builder.CreateMemCpy(sret_arg, al, src, al, sz);
    } else if (tail_val && struct_ty && tail_val->getType() == struct_ty) {
      builder.CreateStore(tail_val, sret_arg);
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
             llvm::cast<llvm::StructType>(ret_type)->getNumElements() == 2 &&
             llvm::cast<llvm::StructType>(ret_type)
                 ->getElementType(0)
                 ->isIntegerTy(8) &&
             llvm::cast<llvm::StructType>(ret_type)
                 ->getElementType(1)
                 ->isArrayTy()) {
    // Union tail. Either tail_val already points at this union alloca, or
    // it's a concrete/error value that must be wrapped into the union
    // (e.g. `fn f() int | error { NetworkError{...} }`).
    llvm::Value *union_val = nullptr;
    if (auto *ai = llvm::dyn_cast<llvm::AllocaInst>(tail_val))
      if (ai->getAllocatedType() == ret_type)
        union_val = builder.CreateLoad(ret_type, tail_val, "ret.union");
    if (!union_val) {
      TypePtr ret_sem = fn.signature.return_type
                            ? semantic_type(*fn.signature.return_type)
                            : nullptr;
      TypePtr tail_sem = block.stmts.empty()
                             ? nullptr
                             : semantic_type(*block.stmts.back());
      if (tail_sem && ret_sem && ret_sem->kind == TypeKind::Union)
        if (auto *wrapped = emit_union_wrap(tail_val, tail_sem, ret_sem))
          union_val = builder.CreateLoad(ret_type, wrapped, "ret.union");
    }
    builder.CreateRet(union_val ? union_val
                                : llvm::Constant::getNullValue(ret_type));
  } else {
    builder.CreateRet(llvm::Constant::getNullValue(ret_type));
  }
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

llvm::Value *CodeGen::emit_empty_array(const TypePtr &array_sem) {
  auto &arr_info = std::get<ArrayTypeInfo>(array_sem->detail);
  int64_t elem_size = 8;
  if (arr_info.element) {
    auto *elem_ll = llvm_type(arr_info.element);
    if (elem_ll->isIntegerTy(1))
      elem_size = 1;
  }
  return builder.CreateCall(
      module->getFunction("saga_array_new"),
      {llvm::ConstantInt::get(i64_type, elem_size),
       llvm::ConstantInt::get(i64_type, 4)}, "arr");
}

llvm::Value *CodeGen::emit_empty_map(const TypePtr &map_sem) {
  auto &map_info = std::get<MapTypeInfo>(map_sem->detail);
  int64_t key_size = 8, val_size = 8;
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
  int64_t key_kind_tag = static_cast<int64_t>(CodeGen::key_kind_for(map_info.key));
  return builder.CreateCall(
      module->getFunction("saga_map_new"),
      {llvm::ConstantInt::get(i64_type, key_size),
       llvm::ConstantInt::get(i64_type, val_size),
       llvm::ConstantInt::get(i64_type, key_kind_tag),
       get_or_emit_key_ops(map_info.key)}, "map");
}

// A union with no initializer zeroes to tag 0 (the leftmost alternative). For a
// reference-typed leftmost, the zeroed payload is a null pointer that would
// crash on use, so materialize its real empty value (`""` / `[]` / `{}`).
void CodeGen::emit_union_leftmost_zero(llvm::Value *alloca,
                                       const TypePtr &union_sem) {
  auto &info = std::get<UnionTypeInfo>(union_sem->detail);
  if (info.alternatives.empty())
    return;
  auto lead = unwrap_alias(info.alternatives[0]);
  llvm::Value *zv = nullptr;
  if (lead && lead->kind == TypeKind::String)
    zv = make_string_constant("");
  else if (lead && lead->kind == TypeKind::Array)
    zv = emit_empty_array(lead);
  else if (lead && lead->kind == TypeKind::Map)
    zv = emit_empty_map(lead);
  if (!zv)
    return; // scalar / struct / void leftmost: zeroed payload is already correct
  auto *union_st = get_union_llvm_type(union_sem);
  auto *payload = builder.CreateStructGEP(union_st, alloca, 1, "u.zero.payload");
  builder.CreateStore(zv, payload);
}

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
      auto sem_type = lookup_sem_type(**node.type);
      var_type = llvm_type(sem_type);
    }
  } else if (node.init) {
    // Infer from the init expression's semantic type.
    if (auto recorded = semantic_type(**node.init))
      var_type = llvm_type(recorded);
  }

  // A `void` variable holds only `null` (no data), but native LLVM void is
  // unstorable — give it a 1-byte slot matching emit_null_literal's placeholder.
  if (var_type->isVoidTy())
    var_type = llvm::Type::getInt8Ty(context);

  // Determine semantic type for refcount tracking and interface boxing.
  TypePtr sem_type_ptr = nullptr;
  if (node.type) {
    // Look up from the node_types map first (recorded during analysis).
    sem_type_ptr = semantic_type(**node.type);
    if (!sem_type_ptr) {
      // Fall back to resolve_type (works for builtins).
      sem_type_ptr = lookup_sem_type(**node.type);
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
      // Errors are boxed (pointer rep); bind the box pointer, don't copy the
      // struct by value (which would overflow an 8-byte union payload later).
      bool boxed_error =
          sem && sem->kind == TypeKind::Struct &&
          std::get<StructTypeInfo>(sem->detail).is_error;
      if (val && sem && sem->kind == TypeKind::Struct && !boxed_error) {
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
      auto *alloca = create_entry_alloca(func, name, var_type);
      locals[name] = alloca;
      builder.CreateStore(emit_empty_array(sem_type_ptr), alloca);
    } else if (sem_type_ptr && sem_type_ptr->kind == TypeKind::Map) {
      auto *alloca = create_entry_alloca(func, name, var_type);
      locals[name] = alloca;
      builder.CreateStore(emit_empty_map(sem_type_ptr), alloca);
    } else if (sem_type_ptr && sem_type_ptr->kind == TypeKind::Union) {
      // Zero = tag 0 (leftmost); materialize a reference-typed leftmost's value.
      auto *alloca = create_entry_alloca(func, name, var_type);
      locals[name] = alloca;
      builder.CreateStore(llvm::Constant::getNullValue(var_type), alloca);
      emit_union_leftmost_zero(alloca, sem_type_ptr);
    } else if (sem_type_ptr && sem_type_ptr->kind == TypeKind::Struct &&
               !std::get<StructTypeInfo>(sem_type_ptr->detail).is_error) {
      // Struct zero value: allocate struct, zero-initialize all fields.
      // (Errors are boxed pointers — they fall to the scalar path below,
      // yielding a null box pointer.)
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

  // ── Single value assignment ──────────────────────────────────────────
  for (auto &ident : node.targets.identifiers) {
    std::string name(ident.name);

    // Struct values: copy into a fresh local alloca to preserve value
    // semantics under D1 ABI. Source may be a pointer (alloca/sret slot)
    // or an SSA struct value.
    {
      auto sem = semantic_type(*node.value);
      // Errors are boxed (llvm_type is a pointer), so they bind like a
      // string/array local — the box pointer is stored, not copied by value.
      bool boxed_error =
          sem && sem->kind == TypeKind::Struct &&
          std::get<StructTypeInfo>(sem->detail).is_error;
      if (val && sem && sem->kind == TypeKind::Struct && !boxed_error) {
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

    if (std::holds_alternative<SelectorNode>(node.targets[i]->data)) {
      emit_field_assign(*node.targets[i], node.op, rhs);
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
      // Reassigning a union variable to a bare member value: wrap it so the
      // tag is set (mirrors the var-decl / struct-field union stores).
      if (target_sem && target_sem->kind == TypeKind::Union) {
        auto val_sem = semantic_type(*node.values[i]);
        if (val_sem && val_sem->kind != TypeKind::Union) {
          if (auto *wrapped = emit_union_wrap(rhs, val_sem, target_sem);
              wrapped && wrapped->getType()->isPointerTy()) {
            auto *ut = alloca->getAllocatedType();
            auto &dl = module->getDataLayout();
            auto al = dl.getABITypeAlign(ut);
            builder.CreateMemCpy(alloca, al, wrapped, al,
                                 dl.getTypeAllocSize(ut));
            continue;
          }
        }
      }
      // Release old value before overwriting if managed.
      if (target_sem && (target_sem->kind == TypeKind::String ||
                         target_sem->kind == TypeKind::Array ||
                         target_sem->kind == TypeKind::Map)) {
        auto *old = builder.CreateLoad(alloca->getAllocatedType(), alloca);
        emit_release(old, target_sem);
      }
      builder.CreateStore(rhs, alloca);
    } else {
      auto *cur = builder.CreateLoad(alloca->getAllocatedType(), alloca);
      builder.CreateStore(emit_compound_op(node.op, cur, rhs, target_sem),
                          alloca);
    }
  }
}

std::pair<llvm::Value *, llvm::Type *>
CodeGen::assign_target_address(const Node &target) {
  if (auto *ident = std::get_if<IdentifierNode>(&target.data)) {
    auto local_it = locals.find(std::string(ident->name));
    if (local_it == locals.end())
      return {nullptr, nullptr};
    return {local_it->second, local_it->second->getAllocatedType()};
  }

  if (auto *sel = std::get_if<SelectorNode>(&target.data)) {
    auto [obj_addr, obj_sem] = struct_lvalue(*sel->object);
    if (!obj_addr)
      return {nullptr, nullptr};
    return struct_field_gep(obj_addr, obj_sem, std::string(sel->field.name));
  }

  return {nullptr, nullptr};
}

void CodeGen::emit_field_assign(const Node &target, Token::Kind op,
                                llvm::Value *rhs) {
  auto [addr, ftype] = assign_target_address(target);
  if (!addr)
    return;

  if (op == Token::Kind::Assignment) {
    builder.CreateStore(rhs, addr);
    return;
  }

  auto *cur = builder.CreateLoad(ftype, addr);
  builder.CreateStore(emit_compound_op(op, cur, rhs, semantic_type(target)),
                      addr);
}

llvm::Value *CodeGen::emit_compound_op(Token::Kind op, llvm::Value *cur,
                                       llvm::Value *rhs,
                                       const TypePtr &target_sem) {
  using K = Token::Kind;

  if (target_sem && target_sem->kind == TypeKind::String) {
    if (op != K::AddAssignment)
      return rhs;
    auto *concat_fn = module->getFunction("saga_string_concat");
    auto *joined = builder.CreateCall(concat_fn, {cur, rhs}, "concat");
    emit_release(cur, target_sem);
    return joined;
  }

  if (cur->getType()->isDoubleTy()) {
    if (rhs->getType()->isIntegerTy(64))
      rhs = builder.CreateSIToFP(rhs, f64_type, "itof");
    switch (op) {
    case K::AddAssignment: return builder.CreateFAdd(cur, rhs, "fadd");
    case K::SubAssignment: return builder.CreateFSub(cur, rhs, "fsub");
    case K::MulAssignment: return builder.CreateFMul(cur, rhs, "fmul");
    case K::DivAssignment: return builder.CreateFDiv(cur, rhs, "fdiv");
    default: return rhs;
    }
  }

  switch (op) {
  case K::AddAssignment: return builder.CreateAdd(cur, rhs, "add");
  case K::SubAssignment: return builder.CreateSub(cur, rhs, "sub");
  case K::MulAssignment: return builder.CreateMul(cur, rhs, "mul");
  case K::DivAssignment: return builder.CreateSDiv(cur, rhs, "div");
  default: return rhs;
  }
}

void CodeGen::emit_return(const ReturnNode &node) {
  if (current_func_is_main) {
    emit_release_locals();
    if (has_spawn)
      builder.CreateCall(module->getFunction("saga_executor_shutdown"), {});
    if (!node.value) {
      builder.CreateRet(
          llvm::ConstantInt::get(llvm::Type::getInt32Ty(context), 0));
    } else {
      auto *val = emit_expr(*node.value);
      auto *i32_val = builder.CreateTrunc(val, llvm::Type::getInt32Ty(context),
                                          "main_ret");
      builder.CreateRet(i32_val);
    }
    return;
  }

  if (!node.value) {
    emit_release_locals();
    builder.CreateRetVoid();
  } else {
    auto *val = emit_expr(*node.value);
    auto *func = builder.GetInsertBlock()->getParent();
    auto *ret_type = func->getReturnType();

    // Sret return: copy struct value/alloca into the hidden first arg.  For a
    // union return, a bare/error value is first wrapped into union memory.
    if (ret_type->isVoidTy() && func->arg_size() > 0 &&
        func->getArg(0)->hasStructRetAttr()) {
      auto *sret_arg = func->getArg(0);
      auto *struct_ty = func->getParamStructRetType(0);
      llvm::Value *src = val;
      if (auto union_sem = union_sem_for_llvm(struct_ty))
        src = as_union_ptr(val, semantic_type(*node.value), union_sem);
      if (src && struct_ty) {
        if (src->getType()->isPointerTy()) {
          auto sz = module->getDataLayout().getTypeAllocSize(struct_ty);
          auto al = module->getDataLayout().getABITypeAlign(struct_ty);
          builder.CreateMemCpy(sret_arg, al, src, al, sz);
        } else if (src->getType() == struct_ty) {
          builder.CreateStore(src, sret_arg);
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
        auto val_sem = semantic_type(*node.value);
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
  }
}

void CodeGen::emit_step(const Node &target, bool increment) {
  auto [addr, type] = assign_target_address(target);
  if (!addr)
    return;

  auto *cur = builder.CreateLoad(type, addr);
  auto *one = llvm::ConstantInt::get(i64_type, 1);
  builder.CreateStore(increment ? builder.CreateAdd(cur, one, "inc")
                                : builder.CreateSub(cur, one, "dec"),
                      addr);
}

void CodeGen::emit_increment(const IncrementNode &node) {
  emit_step(*node.operand, /*increment=*/true);
}

void CodeGen::emit_decrement(const DecrementNode &node) {
  emit_step(*node.operand, /*increment=*/false);
}


} // namespace saga

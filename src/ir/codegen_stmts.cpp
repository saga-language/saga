// Copyright 2026 Rob Thornton
// SPDX-License-Identifier: MIT

#include "ir/codegen.hpp"
#include "util/internal_error.hpp"

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
          align_of(r_ll));
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
            align_of(p_ll));
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

  // Skip the hidden sret arg if present. The function is the authority, not
  // the annotation: a monomorphised specialisation returns a struct by
  // pointer, and re-resolving `fn`'s declared return type cannot tell that
  // apart from a declared function's sret lowering.
  size_t arg_idx = 0;
  bool has_sret =
      !is_main && func->hasParamAttribute(0, llvm::Attribute::StructRet);
  if (has_sret)
    ++arg_idx;

  // Create allocas for parameters and store the incoming argument values.
  // param_ll has one entry per flattened parameter name so variadic /
  // multi-name params are already expanded.
  //
  // An array parameter is a binding, so its slot owns a reference the caller
  // took for it (emit_call_expr) and this frame gives back on the way out.
  size_t ll_idx = 0;
  for (auto &param : fn.signature.params) {
    auto param_sem = lookup_sem_type(*param.type);
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
        auto sz = size_of(ll_type);
        auto al = align_of(ll_type);
        builder.CreateMemCpy(alloca, al, arg, al, sz);
      } else {
        builder.CreateStore(arg, alloca);
      }
      locals[pname] = alloca;
      if (param_sem && param_sem->kind == TypeKind::Array)
        track_managed(alloca, param_sem);
      ++ll_idx;
    }
  }
  (void)has_sret;

  // Emit body.
  auto &block = std::get<BlockNode>(fn.body->data);
  auto *tail_val = emit_block(block);

  // If the block didn't already terminate, release locals and return.
  if (!builder.GetInsertBlock()->getTerminator()) {
    // The tail expression is the return value, so it has to survive the
    // release of the locals it may well be one of.
    if (!is_main && !block.stmts.empty())
      retain_if_borrowed(tail_val, block_result_type(block),
                         *block.stmts.back());
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

  verify_function(*func);
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
      src = as_union_ptr(tail_val, block_result_type(block), union_sem);
    }
    if (src && struct_ty && struct_ty->isStructTy() &&
        src->getType()->isPointerTy()) {
      auto sz = size_of(struct_ty);
      auto al = align_of(struct_ty);
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
        if (auto *wrapped = coerce_to(tail_val, tail_sem, ret_sem))
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
    last = emit_root_expr(*stmt);
  }
  return last;
}

void CodeGen::emit_stmt(const Node &node) {
  std::visit(
      overloaded{
          [&](const VarDeclNode &n) { emit_var_decl(n); },
          [&](const DeclAssignNode &n) { emit_decl_assign(n); },
          [&](const DestructureNode &n) { emit_destructure(n); },
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
      key_size = size_of(key_ll);
    else if (key_ll->isIntegerTy(1))
      key_size = 1;
  }
  if (map_info.value) {
    auto *val_ll = llvm_type(map_info.value);
    if (val_ll->isStructTy())
      val_size = size_of(val_ll);
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

// The language's zero value, which is not always LLVM's: a reference-typed
// zero is the empty container, not the null pointer that would crash on use.
void CodeGen::zero_fill(llvm::Value *slot, const TypePtr &sem,
                        llvm::Type *ll) {
  if (sem && sem->kind == TypeKind::String) {
    builder.CreateStore(make_string_constant(""), slot);
  } else if (sem && sem->kind == TypeKind::Array) {
    builder.CreateStore(emit_empty_array(sem), slot);
  } else if (sem && sem->kind == TypeKind::Map) {
    builder.CreateStore(emit_empty_map(sem), slot);
  } else {
    builder.CreateStore(llvm::Constant::getNullValue(ll), slot);
    if (sem && sem->kind == TypeKind::Union)
      emit_union_leftmost_zero(slot, sem);
  }
}

void CodeGen::emit_var_decl(const VarDeclNode &node) {
  // Every VarDeclNode the parser builds carries a type node — `x := 1` is a
  // DeclAssignNode and lowers through emit_decl_assign — so there is nothing
  // here to infer from an initialiser, and no type worth defaulting to.
  if (!node.type || !*node.type)
    internal_error("a variable declaration reached code generation with no "
                   "type node");

  // The analyzer records the resolved type on the annotation; resolve_type
  // covers the builtins it does not record.
  TypePtr sem = semantic_type(**node.type);
  if (!sem)
    sem = lookup_sem_type(**node.type);

  std::string name(node.name.name);
  if (!node.init) {
    emit_zeroed_local(name, sem);
    return;
  }

  auto *val = emit_root_expr(**node.init);
  retain_if_borrowed(val, sem, **node.init);
  bind_local(name, coerce_to(val, root_expr_type(**node.init), sem), sem);
}

void CodeGen::emit_zeroed_local(const std::string &name, const TypePtr &sem) {
  auto *slot_ll = local_slot_type(sem, nullptr);
  auto *slot =
      create_entry_alloca(builder.GetInsertBlock()->getParent(), name, slot_ll);
  locals[name] = slot;
  zero_fill(slot, sem, slot_ll);
  track_managed(slot, sem);
}

void CodeGen::emit_decl_assign(const DeclAssignNode &node) {
  auto *val = emit_root_expr(*node.value);
  auto sem = materialize_untyped(root_expr_type(*node.value));
  retain_if_borrowed(val, sem, *node.value);

  for (auto &ident : node.targets.identifiers) {
    std::string name(ident.name);
    bind_local(name, val, sem);

    // If a pending channel alloca exists from a spawn expression,
    // create a companion local "<name>.channel" for for-range iteration.
    if (pending_channel_alloca_) {
      std::string ch_name = name + ".channel";
      locals[ch_name] = pending_channel_alloca_;
      pending_channel_alloca_ = nullptr;
    }
  }
}

// Each name is bound the way `x := value.field` binds one, so a struct field
// is copied into its own slot rather than aliasing the value's storage.
void CodeGen::emit_destructure(const DestructureNode &node) {
  // unwrap_alias, because a nominal alias of a struct is one to take apart —
  // the analyzer resolves the fields through it, so this must reach the same
  // struct or it binds nothing at all.
  auto sem = unwrap_alias(root_expr_type(*node.value));
  auto *value = emit_root_expr(*node.value);
  if (!value || !sem || sem->kind != TypeKind::Struct)
    return;

  auto *base = spill_aggregate(value, "destructure.src");
  auto *func = builder.GetInsertBlock()->getParent();

  for (auto &f : node.fields) {
    std::string name(std::get<IdentifierNode>(f.name->data).name);
    auto [gep, field_ll] =
        struct_field_gep(base, sem, std::string(f.field.name));
    if (!gep || !field_ll)
      continue;

    auto *slot = create_entry_alloca(func, name, field_ll);
    if (field_ll->isStructTy()) {
      auto al = align_of(field_ll);
      builder.CreateMemCpy(slot, al, gep, al, size_of(field_ll));
    } else {
      builder.CreateStore(builder.CreateLoad(field_ll, gep, name), slot);
    }
    locals[name] = slot;
    track_managed(slot, semantic_type(*f.name));
  }
}

// A call returning a struct hands back the address of its sret slot, and a
// struct literal hands back its alloca — neither is a loaded value, so a plain
// store would write the pointer into the slot's first field.
void CodeGen::store_into_slot(llvm::Value *slot, llvm::Type *slot_ll,
                              llvm::Value *value) {
  if (slot_ll && slot_ll->isStructTy() && value->getType()->isPointerTy()) {
    auto al = align_of(slot_ll);
    builder.CreateMemCpy(slot, al, value, al, size_of(slot_ll));
    return;
  }
  builder.CreateStore(value, slot);
}

// `saga_map_set` writes in place and stays that way — it is the stdlib's own
// path through `map.Set`. Copy-on-write is the caller's to apply, so the map
// the write lands in is made unique first.
void CodeGen::emit_map_index_assign(const IndexExprNode &target,
                                    const TypePtr &obj_sem, llvm::Value *rhs,
                                    const TypePtr &rhs_sem) {
  auto &info = std::get<MapTypeInfo>(obj_sem->detail);
  auto *map = make_binding_unique(*target.object, obj_sem);
  auto *key = emit_expr(*target.index);
  if (!map || !key)
    return;

  auto *key_slot = collection_slot_address(llvm_type(info.key), info.key, key,
                                           semantic_type(*target.index));
  auto *val_slot =
      collection_slot_address(llvm_type(info.value), info.value, rhs, rhs_sem);
  if (!key_slot || !val_slot)
    return;

  builder.CreateCall(module->getFunction("saga_map_set"),
                     {map, key_slot, val_slot});
}

// A shared backing buffer is copied on write, so `saga_array_set` hands back
// the array to keep, and it has to replace the one the object named. Dropping
// that result is what made the write vanish.
void CodeGen::emit_array_index_assign(const IndexExprNode &target,
                                      const TypePtr &obj_sem, llvm::Value *rhs,
                                      const TypePtr &rhs_sem) {
  auto [holder, holder_ll] = assign_target_address(*target.object);
  if (!holder)
    return;

  auto &info = std::get<ArrayTypeInfo>(obj_sem->detail);
  auto *idx = emit_expr(*target.index);
  auto *elem = collection_slot_address(llvm_type(info.element), info.element,
                                       rhs, rhs_sem);
  if (!idx || !elem)
    return;

  auto *arr = builder.CreateLoad(holder_ll, holder, "arr.cur");
  builder.CreateStore(builder.CreateCall(module->getFunction("saga_array_set"),
                                         {arr, idx, elem}, "arr.set"),
                      holder);
  // `saga_array_set` hands back its own +1, whether it cloned or wrote in
  // place, so the reference the slot held before this is one too many.
  emit_release(arr, obj_sem);
}

void CodeGen::emit_index_assign(const IndexExprNode &target, llvm::Value *rhs,
                                const TypePtr &rhs_sem) {
  auto obj_sem = unwrap_alias(semantic_type(*target.object));
  if (!obj_sem)
    return;
  if (obj_sem->kind == TypeKind::Map)
    emit_map_index_assign(target, obj_sem, rhs, rhs_sem);
  else if (obj_sem->kind == TypeKind::Array)
    emit_array_index_assign(target, obj_sem, rhs, rhs_sem);
}

void CodeGen::emit_assign(const AssignNode &node) {
  for (size_t i = 0; i < node.targets.size() && i < node.values.size(); ++i) {
    auto *rhs = emit_root_expr(*node.values[i]);
    if (!rhs)
      continue;
    auto rhs_sem = root_expr_type(*node.values[i]);
    retain_if_borrowed(rhs, rhs_sem, *node.values[i]);

    if (auto *idx_expr = std::get_if<IndexExprNode>(&node.targets[i]->data))
      emit_index_assign(*idx_expr, rhs, rhs_sem);
    else
      emit_slot_assign(*node.targets[i], node.op, rhs, rhs_sem);
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

void CodeGen::emit_slot_assign(const Node &target, Token::Kind op,
                               llvm::Value *rhs, const TypePtr &rhs_sem) {
  auto [addr, slot_ll] = assign_target_address(target);
  if (!addr)
    return;

  auto target_sem = semantic_type(target);
  if (op == Token::Kind::Assignment) {
    auto *placed = coerce_to(rhs, rhs_sem, target_sem);
    release_slot(addr, slot_ll, target_sem);
    store_into_slot(addr, slot_ll, placed);
    return;
  }

  auto *cur = builder.CreateLoad(slot_ll, addr);
  builder.CreateStore(emit_compound_op(op, cur, rhs, target_sem), addr);
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
    auto *val = emit_root_expr(*node.value);
    // Handed to the caller, so it outlives the release of this frame's locals.
    retain_if_borrowed(val, root_expr_type(*node.value), *node.value);
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
        src = as_union_ptr(val, root_expr_type(*node.value), union_sem);
      if (src && struct_ty) {
        if (src->getType()->isPointerTy()) {
          auto sz = size_of(struct_ty);
          auto al = align_of(struct_ty);
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
        auto val_sem = root_expr_type(*node.value);
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
          auto *wrapped = coerce_to(val, val_sem, ret_sem);
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

// Copyright 2026 Rob Thornton
// SPDX-License-Identifier: MIT

#include "ir/codegen.hpp"
#include "util/internal_error.hpp"

#include <llvm/IR/Constants.h>
#include <llvm/IR/Verifier.h>

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

// ===========================================================================
// Function body emission
// ===========================================================================

void CodeGen::emit_func_decl(const FuncDeclNode &fn) {
  // Receiver methods and generic functions have bodies emitted elsewhere.
  if (fn.is_extern || fn.generic || fn.receiver)
    return;

  bool is_main = fn.name.name == "Main";
  auto *func = module->getFunction(free_func_link_name(fn));
  if (!func)
    return; // Should have been forward-declared.

  return_sems_[func] =
      is_main ? nullptr : declared_return_sem(fn.signature.return_type);
  emit_function_body_inner(fn, func, decl_signature(fn), is_main);
}

void CodeGen::emit_function_body_inner(const FuncDeclNode &fn,
                                       llvm::Function *func,
                                       const FuncTypeInfo &fi, bool is_main) {
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

  bind_params(func, first_param_index(func, false), fn.signature, fi);

  // Emit body.
  auto &block = std::get<BlockNode>(fn.body->data);
  auto *tail_val = emit_block(block);

  if (!builder.GetInsertBlock()->getTerminator()) {
    if (is_main)
      emit_main_exit(nullptr);
    else
      emit_fallthrough_return(block, tail_val);
  }

  verify_function(*func);
}

// A struct receiver holds the caller's address, not a copy of it: there is no
// second spelling to choose between, so a method that writes a field has to
// reach the value the caller named. `struct_slot_address` loads a slot that
// holds a pointer, which is what makes the reads work unchanged. Any other
// receiver arrives as its value.
void CodeGen::emit_receiver_method_body(const FuncDeclNode &fn,
                                        llvm::Function *func,
                                        const FuncTypeInfo &fi) {
  auto *entry = llvm::BasicBlock::Create(context, "entry", func);
  builder.SetInsertPoint(entry);
  locals.clear();
  managed_locals.clear();
  current_func_is_main = false;

  unsigned self_idx = first_param_index(func, false);
  std::string recv_name(fn.receiver->name.name);
  auto *self = func->getArg(self_idx);
  locals[recv_name] = bind_value_slot(func, recv_name, self, self->getType());
  bind_params(func, self_idx + 1, fn.signature, fi);

  auto &block = std::get<BlockNode>(fn.body->data);
  auto *tail_val = emit_block(block);
  if (!builder.GetInsertBlock()->getTerminator())
    emit_fallthrough_return(block, tail_val);
  verify_function(*func);
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
  return emit_new_array(std::get<ArrayTypeInfo>(array_sem->detail).element, 4,
                        "arr");
}

llvm::Value *CodeGen::emit_empty_map(const TypePtr &map_sem) {
  auto &map_info = std::get<MapTypeInfo>(map_sem->detail);
  return emit_new_map(map_info.key, map_info.value);
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
  auto val_sem = root_expr_type(**node.init);
  retain_if_borrowed(val, unwrap_alias(val_sem), **node.init);
  bind_local(name, coerce_to(val, val_sem, sem), sem);
}

void CodeGen::emit_zeroed_local(const std::string &name, const TypePtr &sem) {
  auto *slot_ll = storage_type(unwrap_alias(sem));
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
  bool borrowed = value_ownership(*node.value) == Ownership::Borrowed;

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
    if (borrowed)
      walk_slot(slot, semantic_type(*f.name), true);
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
                                    const TypePtr &rhs_sem,
                                    const Node &rhs_node) {
  auto &info = std::get<MapTypeInfo>(obj_sem->detail);
  auto *map = make_binding_unique(*target.object, obj_sem);
  auto *key = emit_expr(*target.index);
  if (!map || !key)
    return;

  auto key_v = stored_value(key, semantic_type(*target.index), info.key);
  auto val_v = stored_value(rhs, rhs_sem, info.value);
  if (!key_v.address || !val_v.address)
    return;

  builder.CreateCall(module->getFunction("saga_map_set"),
                     {map, key_v.address, val_v.address});
  settle_stored(key_v, *target.index, info.key);
  settle_stored(val_v, rhs_node, info.value);
}

// A shared backing buffer is copied on write, so `saga_array_set` hands back
// the array to keep, and it has to replace the one the object named. Dropping
// that result is what made the write vanish.
void CodeGen::emit_array_index_assign(const IndexExprNode &target,
                                      const TypePtr &obj_sem, llvm::Value *rhs,
                                      const TypePtr &rhs_sem,
                                      const Node &rhs_node) {
  auto [holder, holder_ll] = assign_target_address(*target.object);
  if (!holder)
    return;

  auto &info = std::get<ArrayTypeInfo>(obj_sem->detail);
  auto *idx = emit_expr(*target.index);
  auto elem = stored_value(rhs, rhs_sem, info.element);
  if (!idx || !elem.address)
    return;

  auto *arr = builder.CreateLoad(holder_ll, holder, "arr.cur");
  builder.CreateStore(builder.CreateCall(module->getFunction("saga_array_set"),
                                         {arr, idx, elem.address}, "arr.set"),
                      holder);
  // `saga_array_set` hands back its own +1, whether it cloned or wrote in
  // place, so the reference the slot held before this is one too many.
  emit_release(arr, obj_sem);
  settle_stored(elem, rhs_node, info.element);
}

void CodeGen::emit_index_assign(const IndexExprNode &target, llvm::Value *rhs,
                                const TypePtr &rhs_sem, const Node &rhs_node) {
  auto obj_sem = unwrap_alias(semantic_type(*target.object));
  if (!obj_sem)
    return;
  if (obj_sem->kind == TypeKind::Map)
    emit_map_index_assign(target, obj_sem, rhs, rhs_sem, rhs_node);
  else if (obj_sem->kind == TypeKind::Array)
    emit_array_index_assign(target, obj_sem, rhs, rhs_sem, rhs_node);
}

void CodeGen::emit_assign(const AssignNode &node) {
  for (size_t i = 0; i < node.targets.size() && i < node.values.size(); ++i) {
    auto *rhs = emit_root_expr(*node.values[i]);
    if (!rhs)
      continue;
    auto rhs_sem = root_expr_type(*node.values[i]);
    if (auto *idx_expr = std::get_if<IndexExprNode>(&node.targets[i]->data)) {
      emit_index_assign(*idx_expr, rhs, rhs_sem, *node.values[i]);
      continue;
    }
    if (node.op == Token::Kind::Assignment)
      retain_if_borrowed(rhs, rhs_sem, *node.values[i]);
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
  if (auto *view = llvm::dyn_cast<llvm::AllocaInst>(addr))
    if (auto it = narrow_origins_.find(view); it != narrow_origins_.end())
      return emit_narrowed_assign(it->second, view, target, op, rhs, rhs_sem);

  auto target_sem = semantic_type(target);
  if (op == Token::Kind::Assignment) {
    auto *placed = coerce_to(rhs, rhs_sem, target_sem);
    walk_slot(addr, target_sem, false);
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
  if (current_func_is_main)
    emit_main_exit(node.value ? emit_expr(*node.value) : nullptr);
  else if (!node.value)
    emit_return_value(nullptr, nullptr, nullptr);
  else
    emit_return_value(emit_root_expr(*node.value), root_expr_type(*node.value),
                      node.value.get());
}

void CodeGen::emit_main_exit(llvm::Value *code) {
  auto *i32_ll = llvm::Type::getInt32Ty(context);
  auto *status = code ? builder.CreateTrunc(code, i32_ll, "main_ret")
                      : llvm::ConstantInt::get(i32_ll, 0);
  emit_release_locals();
  if (has_spawn)
    builder.CreateCall(module->getFunction("saga_executor_shutdown"), {});
  builder.CreateRet(status);
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

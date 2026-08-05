// Copyright 2026 Rob Thornton
// SPDX-License-Identifier: MIT

// Composite literals: arrays, maps and structs. Error values are struct
// literals too, so their message defaulting and singleton lowering live here.

#include "ir/codegen.hpp"

#include <llvm/IR/Constants.h>

#include <algorithm>
#include <cstdint>

namespace saga {

// ===========================================================================
// Array literals
// ===========================================================================

llvm::Value *CodeGen::emit_array_literal(const ArrayLiteralNode &node) {
  // Determine element size from the semantic type.
  auto sem = semantic_type(
      *reinterpret_cast<const Node *>(&node)); // hack: node is embedded
  // Try to get element type from the first element.
  int64_t elem_size = 8; // default to i64 size
  llvm::Type *elem_ll_type = i64_type;

  if (!node.elements.empty()) {
    auto first_sem = semantic_type(*node.elements[0]);
    if (first_sem) {
      elem_ll_type = llvm_type(first_sem);
      if (elem_ll_type->isStructTy())
        elem_size = size_of(elem_ll_type);
      else if (elem_ll_type->isIntegerTy(1))
        elem_size = 1;
      else
        elem_size = 8;
    }
  }

  // Create the array: saga_array_new(elem_size, initial_cap)
  auto *new_fn = module->getFunction("saga_array_new");
  auto *arr = builder.CreateCall(
      new_fn,
      {llvm::ConstantInt::get(i64_type, elem_size),
       llvm::ConstantInt::get(i64_type,
                              std::max((int64_t)node.elements.size(), (int64_t)4))},
      "arr");

  // Push each element.
  auto *push_fn = module->getFunction("saga_array_builder_push");
  auto *func = builder.GetInsertBlock()->getParent();

  for (auto &elem_node : node.elements) {
    auto *val = emit_expr(*elem_node);
    if (!val)
      continue;

    // saga_array_builder_push takes a void* to the element and memcpy's
    // elem_size bytes from it.  For struct elements we pass the alloca
    // pointer directly; for SSA values we spill to a temp first.
    if (elem_ll_type->isStructTy()) {
      llvm::Value *src = val;
      if (val->getType()->isStructTy()) {
        auto *tmp = create_entry_alloca(func, "elem.tmp", elem_ll_type);
        builder.CreateStore(val, tmp);
        src = tmp;
      }
      builder.CreateCall(push_fn, {arr, src});
    } else {
      auto *tmp = create_entry_alloca(func, "elem.tmp", val->getType());
      builder.CreateStore(val, tmp);
      builder.CreateCall(push_fn, {arr, tmp});
    }
  }

  return arr;
}

// ===========================================================================
// Map literals
// ===========================================================================

llvm::Value *CodeGen::emit_map_literal(const MapLiteralNode &node) {
  // Determine key/value sizes from semantic types.
  int64_t key_size = 8;  // default to i64 size
  int64_t val_size = 8;
  llvm::Type *key_ll_type = i64_type;
  llvm::Type *val_ll_type = i64_type;
  TypePtr key_sem;

  // Get semantic type of the map literal node itself.
  // We look through the entries to determine types.
  if (!node.entries.empty()) {
    key_sem = semantic_type(*node.entries[0].key);
    auto val_sem = semantic_type(*node.entries[0].value);
    if (key_sem) {
      key_ll_type = llvm_type(key_sem);
      if (key_ll_type->isStructTy())
        key_size = size_of(key_ll_type);
      else if (key_ll_type->isIntegerTy(1))
        key_size = 1;
      else
        key_size = 8;
    }
    if (val_sem) {
      val_ll_type = llvm_type(val_sem);
      if (val_ll_type->isStructTy())
        val_size = size_of(val_ll_type);
      else if (val_ll_type->isIntegerTy(1))
        val_size = 1;
      else
        val_size = 8;
    }
  }

  int64_t key_kind_tag =
      static_cast<int64_t>(CodeGen::key_kind_for(key_sem));
  llvm::Constant *ops_ptr = get_or_emit_key_ops(key_sem);

  // Create the map: saga_map_new(key_size, val_size, key_kind, ops)
  auto *new_fn = module->getFunction("saga_map_new");
  auto *map = builder.CreateCall(
      new_fn,
      {llvm::ConstantInt::get(i64_type, key_size),
       llvm::ConstantInt::get(i64_type, val_size),
       llvm::ConstantInt::get(i64_type, key_kind_tag),
       ops_ptr},
      "map");

  // Insert each entry.
  auto *set_fn = module->getFunction("saga_map_set");
  auto *func = builder.GetInsertBlock()->getParent();

  for (auto &entry : node.entries) {
    auto *key_val = emit_expr(*entry.key);
    auto *val_val = emit_expr(*entry.value);
    if (!key_val || !val_val)
      continue;

    // Key spill (struct keys not supported here yet; default scalar path).
    auto *key_tmp = create_entry_alloca(func, "map.key.tmp", key_val->getType());
    builder.CreateStore(key_val, key_tmp);

    // Value: for struct values, pass the struct alloca pointer directly
    // so the runtime memcpy's val_size bytes of struct contents.
    llvm::Value *val_ptr = nullptr;
    if (val_ll_type->isStructTy()) {
      if (val_val->getType()->isStructTy()) {
        auto *tmp = create_entry_alloca(func, "map.val.tmp", val_ll_type);
        builder.CreateStore(val_val, tmp);
        val_ptr = tmp;
      } else {
        val_ptr = val_val; // already a pointer to a struct alloca
      }
    } else {
      auto *tmp = create_entry_alloca(func, "map.val.tmp", val_val->getType());
      builder.CreateStore(val_val, tmp);
      val_ptr = tmp;
    }

    builder.CreateCall(set_fn, {map, key_tmp, val_ptr});
  }

  return map;
}

// ===========================================================================
// Struct literals
// ===========================================================================

llvm::Value *CodeGen::emit_struct_literal(const StructLiteralNode &node,
                                          const Node &parent) {
  // Prefer the analyzer's recorded type for the literal expression — for
  // a generic struct literal like `Box{value: Point{...}}` the parent
  // node carries the instantiation `Box<Point>` (with substituted fields
  // and concrete type_args), whereas `node.type_expr` is just the bare
  // template identifier.  Fall back to the type-expr type for older
  // call sites or non-generic structs.
  auto sem = semantic_type(parent);
  if (!sem || sem->kind != TypeKind::Struct)
    sem = semantic_type(*node.type_expr);
  if (!sem || sem->kind != TypeKind::Struct)
    return nullptr;

  auto &info = std::get<StructTypeInfo>(sem->detail);
  // Materialize the per-instantiation LLVM struct on first use; for
  // non-generic structs this is a cache hit on the type registered by
  // emit_struct_decl/materialize_import.
  llvm_type(sem);
  std::string skey = struct_cache_key(info);
  auto st_it = struct_types.find(skey);
  if (st_it == struct_types.end())
    return nullptr;

  auto *st = st_it->second;
  auto *func = builder.GetInsertBlock()->getParent();

  // A field-less error with a constant message is immutable and identical
  // across constructions, so it shares one rodata global (no heap allocation).
  if (info.is_error && info.fields.size() == 2)
    if (auto msg = const_error_message(node, info))
      return emit_error_singleton(info, *msg);

  // Errors are boxed on the heap (they escape via unions/returns) and carry a
  // type_id in field 0; other structs get a zero-initialised stack alloca.
  llvm::Value *storage = nullptr;
  if (info.is_error) {
    uint64_t size = size_of(st);
    storage = builder.CreateCall(
        module->getFunction("saga_error_alloc"),
        {llvm::ConstantInt::get(i64_type, size)}, info.name + ".box");
    auto *tid_gep = builder.CreateStructGEP(st, storage, 0, "type_id");
    builder.CreateStore(
        llvm::ConstantInt::get(i64_type,
                               static_cast<uint64_t>(error_type_id(info))),
        tid_gep);
  } else {
    storage = create_entry_alloca(func, info.name + ".lit", st);
    builder.CreateStore(llvm::Constant::getNullValue(st), storage);
  }

  // Apply comptime field defaults first (descending embed slots), then the
  // explicit literal fields, which override. Defaults are side-effect-free
  // comptime expressions, so a defaulted-then-provided field just lowers the
  // default and overwrites it.
  apply_struct_field_defaults(storage, sem);

  // Store each provided field. For a literal that addresses a promoted field
  // (the field lives on an embedded struct), struct_field_gep walks the embed
  // layout and hands back a pointer into the right inner slot.
  for (auto &fa : node.fields) {
    std::string fname(fa.name.name);
    auto [gep, field_ll] = struct_field_gep(storage, sem, fname);
    if (!gep)
      continue;
    TypePtr field_sem;
    for (auto &fi : info.fields)
      if (fi.name == fname) { field_sem = fi.type; break; }
    store_struct_field(gep, field_ll, field_sem, *fa.value);
  }

  if (info.is_error) {
    bool msg_provided = false;
    for (auto &fa : node.fields)
      if (fa.name.name == "message") { msg_provided = true; break; }
    if (!msg_provided)
      emit_error_message_default(storage, sem, info);
  }

  // For a boxed error this is the heap box pointer; for a struct, the alloca.
  return storage;
}

// An error's `message = Expr` default may interpolate the error's own fields.
// The referenced fields are already stored in the box, so bind each to a temp
// local for the duration of the message expression and store the result into
// the message slot. Unbound temps are dropped by later passes.
void CodeGen::emit_error_message_default(llvm::Value *box, const TypePtr &sem,
                                         const StructTypeInfo &info) {
  const Node *msg_default = nullptr;
  for (auto &f : info.fields)
    if (f.name == "message") { msg_default = f.default_value; break; }
  if (!msg_default)
    return;

  auto *st = struct_types.at(struct_cache_key(info));
  auto *func = builder.GetInsertBlock()->getParent();
  auto saved = locals;
  for (size_t i = 0; i < info.fields.size() && i < st->getNumElements(); ++i) {
    auto &f = info.fields[i];
    if (f.name == "type_id" || f.name == "message")
      continue;
    auto *field_ll = st->getElementType(i);
    auto *gep = builder.CreateStructGEP(st, box, i, f.name);
    auto *tmp = create_entry_alloca(func, f.name + ".self", field_ll);
    builder.CreateStore(builder.CreateLoad(field_ll, gep, f.name), tmp);
    locals[f.name] = tmp;
  }

  auto [msg_gep, msg_ll] = struct_field_gep(box, sem, "message");
  if (msg_gep)
    store_struct_field(msg_gep, msg_ll, analyzer.builtins.string_type,
                       *msg_default);
  locals = saved;
}

std::optional<std::string>
CodeGen::const_error_message(const StructLiteralNode &node,
                            const StructTypeInfo &info) {
  const Node *msg = nullptr;
  for (auto &fa : node.fields)
    if (fa.name.name == "message") { msg = fa.value.get(); break; }
  if (!msg)
    msg = info.fields[1].default_value;
  if (!msg)
    return std::string();

  auto *sl = std::get_if<StringLiteralNode>(&msg->data);
  if (!sl)
    return std::nullopt;
  std::string text;
  for (auto &frag : sl->fragments) {
    auto *sf = std::get_if<StringFragmentNode>(&frag->data);
    if (!sf)
      return std::nullopt;
    text += unescape_string_fragment(*sf);
  }
  return text;
}

llvm::Value *CodeGen::emit_error_singleton(const StructTypeInfo &info,
                                           const std::string &message) {
  uint64_t tid = error_type_id(info);
  std::string key = std::to_string(tid) + '\x01' + message;
  auto it = error_singletons.find(key);
  if (it != error_singletons.end())
    return it->second;

  auto *st = struct_types.at(struct_cache_key(info));
  auto *msg = llvm::cast<llvm::Constant>(make_string_constant(message));
  auto *init = llvm::ConstantStruct::get(
      st, {llvm::ConstantInt::get(i64_type, tid), msg});
  auto *global = new llvm::GlobalVariable(
      *module, st, /*isConstant=*/true, llvm::GlobalValue::PrivateLinkage, init,
      info.name + ".singleton");
  global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
  error_singletons[key] = global;
  return global;
}

void CodeGen::store_struct_field(llvm::Value *gep, llvm::Type *field_ll,
                                 const TypePtr &field_sem,
                                 const Node &value_node) {
  auto *val = emit_expr(value_node);
  if (!val)
    return;

  // Field is a union; the supplied value is one alternative. Wrap before
  // memcpy so the union's tag is set correctly. Without this an
  // `optional String | Missing` field given `Missing{}` would memcpy zero
  // bytes into a 9-byte slot, leaving the tag at 0 (an empty String).
  if (field_sem && field_sem->kind == TypeKind::Union) {
    auto val_sem = semantic_type(value_node);
    if (val_sem && val_sem->kind != TypeKind::Union) {
      auto *wrapped = emit_union_wrap(val, val_sem, field_sem);
      if (wrapped) val = wrapped;
    }
  }

  // D1: aggregate fields are stored inline. If the rhs is a pointer to a
  // struct (e.g. from a nested struct literal), memcpy the bytes rather than
  // storing the pointer into the struct slot.
  if (field_ll && field_ll->isStructTy() && val->getType()->isPointerTy()) {
    uint64_t sz = size_of(field_ll);
    llvm::Align al = align_of(field_ll);
    builder.CreateMemCpy(gep, al, val, al, sz);
  } else {
    builder.CreateStore(val, gep);
  }
}

void CodeGen::apply_struct_field_defaults(llvm::Value *struct_ptr,
                                          const TypePtr &struct_sem) {
  auto &info = std::get<StructTypeInfo>(struct_sem->detail);
  std::string skey = struct_cache_key(info);
  auto st_it = struct_types.find(skey);
  if (st_it == struct_types.end())
    return;
  auto *st = st_it->second;

  for (size_t i = 0; i < info.fields.size() && i < st->getNumElements(); ++i) {
    if (!info.fields[i].default_value)
      continue;
    // An error's message default may interpolate sibling fields, so it is
    // emitted last (emit_error_message_default) once they are populated.
    if (info.is_error && info.fields[i].name == "message")
      continue;
    auto *gep = builder.CreateStructGEP(st, struct_ptr, i, info.fields[i].name);
    llvm::Type *field_ll = st->getElementType(i);
    if (info.fields[i].type)
      if (auto *sem_ll = llvm_type(info.fields[i].type))
        field_ll = sem_ll;
    store_struct_field(gep, field_ll, info.fields[i].type,
                       *info.fields[i].default_value);
  }

  for (size_t ei = 0; ei < info.embeds.size(); ++ei) {
    auto &embed = info.embeds[ei];
    if (!embed || embed->kind != TypeKind::Struct)
      continue;
    size_t slot = info.fields.size() + ei;
    if (slot >= st->getNumElements())
      break;
    auto &einfo = std::get<StructTypeInfo>(embed->detail);
    auto *eslot =
        builder.CreateStructGEP(st, struct_ptr, slot, embed_slot_name(einfo));
    apply_struct_field_defaults(eslot, embed);
  }
}

} // namespace saga

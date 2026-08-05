// Copyright 2026 Rob Thornton
// SPDX-License-Identifier: MIT

// Selector chain dispatch in code generation: resolves and emits LLVM
// values for module selectors, struct field reads, and enum variant
// tags. Method-call selector dispatch lives in emit_call_expr.

#include "ir/codegen.hpp"

#include <llvm/IR/Constants.h>
#include <llvm/IR/GlobalVariable.h>

namespace saga {

llvm::Value *CodeGen::struct_slot_address(llvm::AllocaInst *slot,
                                          const TypePtr &sem,
                                          const std::string &name) {
  auto &info = std::get<StructTypeInfo>(sem->detail);
  auto st_it = struct_types.find(struct_cache_key(info));
  if (st_it == struct_types.end())
    return nullptr;

  auto *slot_type = slot->getAllocatedType();
  if (slot_type == st_it->second)
    return slot;
  if (slot_type->isPointerTy())
    return builder.CreateLoad(slot_type, slot, name);
  return nullptr;
}

std::pair<llvm::Value *, TypePtr> CodeGen::struct_lvalue(const Node &node) {
  auto sem = semantic_type(node);
  if (!sem || sem->kind != TypeKind::Struct)
    return {nullptr, nullptr};

  if (auto *ident = std::get_if<IdentifierNode>(&node.data)) {
    std::string name(ident->name);
    auto local_it = locals.find(name);
    if (local_it == locals.end())
      return {nullptr, nullptr};
    if (auto *addr = struct_slot_address(local_it->second, sem, name))
      return {addr, sem};
    return {nullptr, nullptr};
  }

  if (auto *sel = std::get_if<SelectorNode>(&node.data)) {
    auto [base, base_sem] = struct_lvalue(*sel->object);
    if (!base)
      return {nullptr, nullptr};
    auto [gep, _] =
        struct_field_gep(base, base_sem, std::string(sel->field.name));
    if (gep)
      return {gep, sem};
  }

  return {nullptr, nullptr};
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
        // materialize_import() has already registered all enum keys.
        if (exp.type && exp.type->kind == TypeKind::Enum) {
          return llvm::ConstantInt::get(i64_type, 0); // sentinel for chained selectors
        }

        // Non-function, non-enum export: a module-level constant.
        // Declare (or find) an external global and load from it.
        // materialize_import() has already registered struct types.
        {
          std::string gv_name = mangle(mod.name, field_name);
          auto *ll = llvm_type(exp.type);

          if (exp.type && exp.type->kind == TypeKind::Struct) {
            auto &sinfo = std::get<StructTypeInfo>(exp.type->detail);
            std::string origin =
                sinfo.origin_package.empty() ? mod.name : sinfo.origin_package;
            std::string skey = mangle(origin, sinfo.name);
            auto st_it = struct_types.find(skey);
            if (st_it != struct_types.end())
              ll = st_it->second;
          }

          auto *gv = module->getGlobalVariable(gv_name);
          if (!gv) {
            gv = new llvm::GlobalVariable(
                *module, ll, /*isConstant=*/true,
                llvm::GlobalValue::ExternalLinkage,
                /*Initializer=*/nullptr, gv_name);
          }
          // For struct-typed constants under D1 ABI, return the pointer so
          // chained selectors can GEP through it. For scalar constants, load.
          if (exp.type && exp.type->kind == TypeKind::Struct)
            return gv;
          return builder.CreateLoad(ll, gv, field_name);
        }
        break;
      }
    }
    return nullptr;
  }

  if (auto [obj_addr, obj_sem] = struct_lvalue(*node.object); obj_addr) {
    auto [gep, ftype] = struct_field_gep(obj_addr, obj_sem, field_name);
    if (gep) {
      if (ftype && ftype->isStructTy())
        return gep;
      return builder.CreateLoad(ftype, gep, field_name);
    }
  }

  // Fallback: emit the object, then GEP into it.
  auto *obj = emit_expr(*node.object);
  if (!obj)
    return nullptr;

  auto sem = semantic_type(*node.object);
  if (sem && sem->kind == TypeKind::Struct) {
    // emit_expr may yield the struct by value (a const-global load or a call
    // result); struct_field_gep needs a pointer, so spill to a temp first.
    if (obj->getType()->isStructTy()) {
      auto *func = builder.GetInsertBlock()->getParent();
      auto *tmp = create_entry_alloca(func, "field.tmp", obj->getType());
      builder.CreateStore(obj, tmp);
      obj = tmp;
    }
    auto [gep, ftype] = struct_field_gep(obj, sem, field_name);
    if (gep) {
      if (ftype && ftype->isStructTy())
        return gep;
      return builder.CreateLoad(ftype, gep, field_name);
    }
  }

  // Enum variant access: EnumName.Variant → integer constant.
  if (sem && sem->kind == TypeKind::Enum) {
    auto &info = std::get<EnumTypeInfo>(sem->detail);
    std::string ekey = key_for(info.origin_package, info.name);
    std::string ev_key = ekey + "." + field_name;
    auto ev_it = enum_variants.find(ev_key);
    if (ev_it != enum_variants.end())
      return llvm::ConstantInt::get(i64_type, ev_it->second);
  }

  // Array/other built-in methods accessed via selector (handled in call).
  return nullptr;
}

// ===========================================================================
// Selector (field access)
// ===========================================================================

std::pair<llvm::Value *, llvm::Type *>
CodeGen::struct_field_gep(llvm::Value *struct_ptr,
                          const TypePtr &struct_sem_type,
                          const std::string &field_name) {
  if (!struct_sem_type || struct_sem_type->kind != TypeKind::Struct)
    return {nullptr, nullptr};

  auto &info = std::get<StructTypeInfo>(struct_sem_type->detail);
  std::string skey = struct_cache_key(info);
  auto st_it = struct_types.find(skey);
  if (st_it == struct_types.end())
    return {nullptr, nullptr};

  auto *st = st_it->second;
  auto &fields = struct_fields[skey];

  // Direct field lookup. We restrict to info.fields.size() so that the
  // synthetic `__embed_<Name>` slots appended after the own fields are
  // not addressable by name from user code — they are reachable only via
  // promoted-field access (handled below).
  for (size_t i = 0; i < info.fields.size() && i < fields.size(); ++i) {
    if (fields[i] == field_name) {
      auto *gep = builder.CreateStructGEP(st, struct_ptr, i, field_name);
      // Prefer the semantic field type's LLVM lowering so generic
      // instantiations (e.g. Box<Int>) read/write at the right element
      // type even when the underlying LLVM struct was emitted with a
      // ptr-typed slot for the unsubstituted template field. Sizes must
      // match the slot for this to be safe; aggregate type arguments
      // wider than a pointer are tracked as P8 tech debt.
      llvm::Type *field_ll = st->getElementType(i);
      if (info.fields[i].type) {
        if (auto *sem_ll = llvm_type(info.fields[i].type))
          field_ll = sem_ll;
      }
      return {gep, field_ll};
    }
  }

  // Embed slots are appended to `fields` after the owner's own fields, in the
  // same order as `info.embeds`. An embed is reachable two ways: by its own
  // type name (`u.Timestamps`), addressing the whole embedded value, and by
  // any member it promotes. Both passes are needed because the name of an
  // embed sits at depth 0 alongside the owner's fields, while anything it
  // promotes is deeper — so a shallower match must win even when a deeper one
  // appears in an earlier embed.
  for (size_t ei = 0; ei < info.embeds.size(); ++ei) {
    auto &embed = info.embeds[ei];
    if (!embed || embed->kind != TypeKind::Struct) continue;
    size_t slot_idx = info.fields.size() + ei;
    if (slot_idx >= fields.size()) break;

    auto &einfo = std::get<StructTypeInfo>(embed->detail);
    if (einfo.name != field_name) continue;
    auto *gep = builder.CreateStructGEP(st, struct_ptr, slot_idx,
                                        embed_slot_name(einfo));
    return {gep, st->getElementType(slot_idx)};
  }

  for (size_t ei = 0; ei < info.embeds.size(); ++ei) {
    auto &embed = info.embeds[ei];
    if (!embed || embed->kind != TypeKind::Struct) continue;
    size_t slot_idx = info.fields.size() + ei;
    if (slot_idx >= fields.size()) break;

    // Promoted access: the member lives somewhere inside this embed.
    auto &einfo = std::get<StructTypeInfo>(embed->detail);
    auto *slot_gep = builder.CreateStructGEP(st, struct_ptr, slot_idx,
                                             embed_slot_name(einfo));
    auto inner = struct_field_gep(slot_gep, embed, field_name);
    if (inner.first) return inner;
  }

  return {nullptr, nullptr};
}

std::pair<llvm::Value *, TypePtr>
CodeGen::embed_method_receiver(llvm::Value *struct_ptr,
                               const TypePtr &struct_sem,
                               const std::string &method) {
  auto &info = std::get<StructTypeInfo>(struct_sem->detail);
  for (auto &m : info.methods)
    if (m.name == method)
      return {struct_ptr, struct_sem};

  std::string skey = struct_cache_key(info);
  auto st_it = struct_types.find(skey);
  if (st_it == struct_types.end())
    return {nullptr, nullptr};
  auto *st = st_it->second;

  for (size_t ei = 0; ei < info.embeds.size(); ++ei) {
    auto &embed = info.embeds[ei];
    if (!embed || embed->kind != TypeKind::Struct) continue;
    size_t slot_idx = info.fields.size() + ei;
    if (slot_idx >= st->getNumElements()) break;

    auto &einfo = std::get<StructTypeInfo>(embed->detail);
    auto *slot_gep = builder.CreateStructGEP(st, struct_ptr, slot_idx,
                                             embed_slot_name(einfo));
    auto inner = embed_method_receiver(slot_gep, embed, method);
    if (inner.first) return inner;
  }

  return {nullptr, nullptr};
}

} // namespace saga

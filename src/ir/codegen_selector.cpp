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

} // namespace saga

// Copyright 2026 Rob Thornton
// SPDX-License-Identifier: MIT

// Selector chain dispatch in code generation: resolves and emits LLVM
// values for module selectors, struct field reads, and enum variant
// tags. Method-call selector dispatch lives in emit_call_expr.

#include "ir/codegen.hpp"

#include <llvm/IR/Constants.h>
#include <llvm/IR/GlobalVariable.h>

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
        std::string skey = key_for(info.origin_package, info.name);
        auto st_it = struct_types.find(skey);
        if (st_it != struct_types.end()) {
          // The alloca might be a ptr to struct (if stored from a literal)
          // or the struct type itself.
          auto *alloca_type = alloca->getAllocatedType();
          if (alloca_type == st_it->second) {
            // Direct struct alloca — GEP into it.
            auto [gep, ftype] = struct_field_gep(alloca, sem, field_name);
            if (gep) {
              if (ftype && ftype->isStructTy())
                return gep;
              return builder.CreateLoad(ftype, gep, field_name);
            }
          } else if (alloca_type->isPointerTy()) {
            // Pointer to struct — load the pointer, then GEP.
            auto *ptr = builder.CreateLoad(alloca_type, alloca, obj_name);
            auto [gep, ftype] = struct_field_gep(ptr, sem, field_name);
            if (gep) {
              if (ftype && ftype->isStructTy())
                return gep;
              return builder.CreateLoad(ftype, gep, field_name);
            }
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

} // namespace mc

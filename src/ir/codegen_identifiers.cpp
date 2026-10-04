// Copyright 2026 Rob Thornton
// SPDX-License-Identifier: MIT

#include "ir/codegen.hpp"

#include <llvm/IR/Constants.h>

namespace saga {

llvm::Value *CodeGen::emit_identifier(const IdentifierNode &node,
                                      const Node &parent) {
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
  // Use the analyzer's symbol table to get the origin package so cross-
  // package enum references resolve without falling back through the local
  // package.
  if (auto sym = analyzer.lookup(name);
      sym && sym->type && sym->type->kind == TypeKind::Enum) {
    auto &info = std::get<EnumTypeInfo>(sym->type->detail);
    if (enum_types.count(key_for(info.origin_package, info.name)))
      return llvm::ConstantInt::get(i64_type, 0);
  }
  if (enum_types.count(key_for("", name)))
    return llvm::ConstantInt::get(i64_type, 0);

  // A shape with no fields may be written without `{}`, so a bare type name
  // here is a construction, not a type escaping into value position. Locals
  // were resolved above, and the analyzer's recorded type is what admitted the
  // braceless form, so it is what decides. There is nothing to store: the slot
  // exists so the value has an address to be passed and received by.
  if (auto sem = semantic_type(parent); is_empty_shape(sem)) {
    llvm_type(sem);
    auto &info = std::get<StructTypeInfo>(sem->detail);
    if (auto it = struct_types.find(struct_cache_key(info));
        it != struct_types.end())
      return create_entry_alloca(builder.GetInsertBlock()->getParent(),
                                 info.name + ".shape", it->second);
  }

  // Top-level function referenced as a value (e.g. `call_it(greet, ...)` or
  // a struct literal like `Reg{ handler: greet }`).  Return the raw LLVM
  // Function* — it is pointer-typed, matching how function-typed locals
  // and struct fields are lowered.
  if (auto *fn = module->getFunction(mangle(name)))
    return fn;

  // Top-level constant declared in the current package.  emit_const_decl
  // creates a GlobalVariable named mangle(name); identifier reads from it.
  // Struct-typed constants return the pointer (caller GEPs through it);
  // scalar/string/array/map constants load the stored value.
  if (auto *gv = module->getGlobalVariable(mangle(name))) {
    auto sym = analyzer.lookup(name);
    if (sym && sym->type && sym->type->kind == TypeKind::Struct)
      return gv;
    return builder.CreateLoad(gv->getValueType(), gv, name);
  }

  return nullptr;
}

} // namespace saga

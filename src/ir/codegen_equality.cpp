// Copyright 2026 Rob Thornton
// SPDX-License-Identifier: MIT

#include "ir/codegen.hpp"

#include <llvm/IR/Constants.h>

namespace saga {

// `a == b` / `a != b` on errors: equal iff same type_id and equal fields. Field
// reads use a concrete layout (from a non-abstract operand; else base `error`)
// and are guarded by the type_id check, so the layout always matches the box.
llvm::Value *CodeGen::emit_error_equality(const BinaryExprNode &node,
                                          const TypePtr &lhs_sem,
                                          const TypePtr &rhs_sem) {
  auto *a = emit_borrowed(*node.lhs);
  auto *b = emit_borrowed(*node.rhs);
  if (!a || !b)
    return nullptr;
  auto *i1 = llvm::Type::getInt1Ty(context);
  auto *func = builder.GetInsertBlock()->getParent();

  auto *tid_eq = builder.CreateICmpEQ(
      builder.CreateLoad(i64_type, a, "a.tid"),
      builder.CreateLoad(i64_type, b, "b.tid"), "err.tid.eq");

  const TypePtr &layout = is_abstract_error(lhs_sem) ? rhs_sem : lhs_sem;

  auto *entry = builder.GetInsertBlock();
  auto *cmp_bb = llvm::BasicBlock::Create(context, "err.eq.fields", func);
  auto *done_bb = llvm::BasicBlock::Create(context, "err.eq.done", func);
  builder.CreateCondBr(tid_eq, cmp_bb, done_bb);

  builder.SetInsertPoint(cmp_bb);
  auto *fields_eq = emit_error_fields_eq(a, b, layout);
  auto *cmp_end = builder.GetInsertBlock();
  builder.CreateBr(done_bb);

  builder.SetInsertPoint(done_bb);
  auto *phi = builder.CreatePHI(i1, 2, "err.eq");
  phi->addIncoming(llvm::ConstantInt::getFalse(i1), entry);
  phi->addIncoming(fields_eq, cmp_end);

  return node.op == Token::Kind::NotEqual ? builder.CreateNot(phi, "err.ne")
                                          : phi;
}

// Compare every field after type_id (message + extras) of two error boxes read
// through `layout`'s struct type. Both boxes are the same concrete type here.
llvm::Value *CodeGen::emit_error_fields_eq(llvm::Value *a, llvm::Value *b,
                                           const TypePtr &layout) {
  return emit_struct_fields_eq(a, b, layout, /*start=*/1);
}

// AND together per-field equality of two same-typed struct pointers, starting
// at field `start` (1 for an error box, skipping type_id; 0 for a plain struct).
llvm::Value *CodeGen::emit_struct_fields_eq(llvm::Value *a, llvm::Value *b,
                                            const TypePtr &struct_sem,
                                            size_t start) {
  llvm_type(struct_sem);
  auto &info = std::get<StructTypeInfo>(unwrap_alias(struct_sem)->detail);
  auto *st = struct_types.at(struct_cache_key(info));
  auto *i1 = llvm::Type::getInt1Ty(context);

  llvm::Value *acc = llvm::ConstantInt::getTrue(i1);
  for (size_t i = start; i < info.fields.size() && i < st->getNumElements();
       ++i) {
    auto *fa = builder.CreateStructGEP(st, a, i, "a.f");
    auto *fb = builder.CreateStructGEP(st, b, i, "b.f");
    acc = builder.CreateAnd(
        acc, emit_value_eq(info.fields[i].type, st->getElementType(i), fa, fb),
        "f.and");
  }
  return acc;
}

llvm::Value *CodeGen::emit_value_eq(const TypePtr &field_type,
                                    llvm::Type *field_ll, llvm::Value *a_gep,
                                    llvm::Value *b_gep) {
  auto k = field_type ? unwrap_alias(field_type)->kind : TypeKind::Int;
  if (k == TypeKind::String) {
    auto *cmp = builder.CreateCall(
        module->getFunction("saga_string_compare"),
        {builder.CreateLoad(field_ll, a_gep, "a.v"),
         builder.CreateLoad(field_ll, b_gep, "b.v")}, "f.strcmp");
    return builder.CreateICmpEQ(cmp, llvm::ConstantInt::get(i64_type, 0),
                                "f.streq");
  }
  // A nested struct-by-value field compares recursively so its own string /
  // managed fields are compared by value, not by raw bytes.
  if (k == TypeKind::Struct)
    return emit_struct_fields_eq(a_gep, b_gep, field_type, /*start=*/0);
  // Other aggregates (by-value arrays) fall back to a raw byte compare.
  if (field_ll->isAggregateType()) {
    uint64_t size = size_of(field_ll);
    auto *cmp = builder.CreateCall(
        get_or_declare_memcmp(),
        {a_gep, b_gep, llvm::ConstantInt::get(i64_type, size)}, "f.memcmp");
    return builder.CreateICmpEQ(cmp, llvm::ConstantInt::get(cmp->getType(), 0),
                                "f.memeq");
  }
  auto *av = builder.CreateLoad(field_ll, a_gep, "a.v");
  auto *bv = builder.CreateLoad(field_ll, b_gep, "b.v");
  if (k == TypeKind::Float)
    return builder.CreateFCmpOEQ(av, bv, "f.feq");
  return builder.CreateICmpEQ(av, bv, "f.eq");
}

llvm::Function *CodeGen::get_or_declare_memcmp() {
  if (auto *f = module->getFunction("memcmp"))
    return f;
  auto *ft = llvm::FunctionType::get(
      llvm::Type::getInt32Ty(context),
      {llvm::PointerType::getUnqual(context),
       llvm::PointerType::getUnqual(context), i64_type},
      /*isVarArg=*/false);
  return llvm::Function::Create(ft, llvm::GlobalValue::ExternalLinkage,
                                "memcmp", module.get());
}

} // namespace saga

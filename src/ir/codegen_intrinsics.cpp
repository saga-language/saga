// Copyright 2026 Rob Thornton
// SPDX-License-Identifier: MIT

// The `intrinsic_*` builtins the stdlib is written on. Each lowers to inline
// instructions or a runtime call rather than to a Saga function.

#include "ir/codegen.hpp"

#include <llvm/IR/Constants.h>
#include <llvm/IR/InlineAsm.h>

namespace saga {

namespace {
struct IntWidth {
  unsigned bits;
  bool is_signed;
};

const std::unordered_map<std::string_view, IntWidth> int_width_intrinsics = {
    {"intrinsic_sext_i8", {8, true}},    {"intrinsic_sext_i16", {16, true}},
    {"intrinsic_sext_i32", {32, true}},  {"intrinsic_sext_i64", {64, true}},
    {"intrinsic_zext_u8", {8, false}},   {"intrinsic_zext_u16", {16, false}},
    {"intrinsic_zext_u32", {32, false}}, {"intrinsic_zext_u64", {64, false}},
};
} // namespace

std::optional<llvm::Value *>
CodeGen::emit_intrinsic_call(const std::string &name,
                             const CallExprNode &node) {
  using Emitter = llvm::Value *(CodeGen::*)(const CallExprNode &);
  static const std::unordered_map<std::string_view, Emitter> emitters = {
      {"intrinsic_sitofp", &CodeGen::emit_intrinsic_sitofp},
      {"intrinsic_sitofp32", &CodeGen::emit_intrinsic_sitofp32},
      {"intrinsic_fptrunc", &CodeGen::emit_intrinsic_fptrunc},
      {"intrinsic_fpext", &CodeGen::emit_intrinsic_fpext},
      {"intrinsic_fptosi", &CodeGen::emit_intrinsic_fptosi},
      {"intrinsic_is_string", &CodeGen::emit_intrinsic_is_string},
      {"intrinsic_yield", &CodeGen::emit_intrinsic_yield},
      {"intrinsic_atomic_add", &CodeGen::emit_intrinsic_atomic_add},
      {"intrinsic_trap", &CodeGen::emit_intrinsic_trap},
      {"intrinsic_syscall", &CodeGen::emit_intrinsic_syscall},
      {"intrinsic_ptr", &CodeGen::emit_intrinsic_ptr},
  };
  if (auto it = int_width_intrinsics.find(name);
      it != int_width_intrinsics.end())
    return emit_int_width(node, it->second.bits, it->second.is_signed);
  if (auto it = emitters.find(name); it != emitters.end())
    return (this->*it->second)(node);
  return std::nullopt;
}

llvm::Value *CodeGen::emit_intrinsic_sitofp(const CallExprNode &node) {
  auto *val = emit_expr(*node.args[0]);
  if (!val) return nullptr;
  return builder.CreateSIToFP(val, f64_type, "sitofp");
}

// A float32 is stored as f64, like every float width: these round to f32
// and extend back, the float side of emit_int_width below.
llvm::Value *CodeGen::emit_intrinsic_sitofp32(const CallExprNode &node) {
  auto *val = emit_expr(*node.args[0]);
  if (!val) return nullptr;
  auto *narrow =
      builder.CreateSIToFP(val, llvm::Type::getFloatTy(context), "sitofp32");
  return builder.CreateFPExt(narrow, f64_type, "f32.wide");
}

llvm::Value *CodeGen::emit_intrinsic_fptrunc(const CallExprNode &node) {
  auto *val = emit_expr(*node.args[0]);
  if (!val) return nullptr;
  auto *narrow =
      builder.CreateFPTrunc(val, llvm::Type::getFloatTy(context), "fptrunc");
  return builder.CreateFPExt(narrow, f64_type, "f32.wide");
}

llvm::Value *CodeGen::emit_intrinsic_fpext(const CallExprNode &node) {
  auto *val = emit_expr(*node.args[0]);
  if (!val) return nullptr;
  auto *dst = llvm::Type::getDoubleTy(context);
  if (val->getType() == dst) return val;
  return builder.CreateFPExt(val, dst, "fpext");
}

llvm::Value *CodeGen::emit_intrinsic_fptosi(const CallExprNode &node) {
  auto *val = emit_expr(*node.args[0]);
  if (!val) return nullptr;
  return builder.CreateFPToSI(val, i64_type, "fptosi");
}

// Trunc-then-extend back to i64 keeps the runtime's uniform i64 storage
// convention for narrow ints.
llvm::Value *CodeGen::emit_int_width(const CallExprNode &node, unsigned bits,
                                     bool is_signed) {
  auto *val = emit_expr(*node.args[0]);
  if (!val) return nullptr;
  if (bits >= 64) return val;
  auto *narrow_ty = llvm::IntegerType::get(context, bits);
  auto *narrow =
      builder.CreateTrunc(val, narrow_ty, is_signed ? "strunc" : "ztrunc");
  return is_signed ? builder.CreateSExt(narrow, i64_type, "sext")
                   : builder.CreateZExt(narrow, i64_type, "zext");
}

// Folds to a constant from the argument's monomorphised static type; LLVM
// drops the dead arm of the branch around it. The argument is still emitted
// so any side effects run.
llvm::Value *CodeGen::emit_intrinsic_is_string(const CallExprNode &node) {
  if (node.args.empty()) return nullptr;
  (void)emit_expr(*node.args[0]);
  auto arg_sem = semantic_type(*node.args[0]);
  bool is_string = arg_sem && arg_sem->kind == TypeKind::String;
  return llvm::ConstantInt::get(i1_type, is_string ? 1 : 0);
}

// The runtime reads the current actor from a thread-local, so this is safe
// at any call depth and a no-op outside an actor.
llvm::Value *CodeGen::emit_intrinsic_yield(const CallExprNode &) {
  builder.CreateCall(module->getFunction("saga_actor_yield"), {});
  return llvm::Constant::getNullValue(llvm::PointerType::getUnqual(context));
}

// The first argument names the i64 to add to, so it is the variable's slot
// that is needed, not its value.
llvm::Value *CodeGen::emit_intrinsic_atomic_add(const CallExprNode &node) {
  auto *val = emit_expr(*node.args[1]);
  auto *ptr_ident = std::get_if<IdentifierNode>(&node.args[0]->data);
  llvm::Value *ptr = nullptr;
  if (ptr_ident) {
    auto it = locals.find(std::string(ptr_ident->name));
    if (it != locals.end())
      ptr = it->second;
  }
  if (!ptr)
    return llvm::Constant::getNullValue(i64_type);
  return builder.CreateAtomicRMW(llvm::AtomicRMWInst::Add, ptr, val,
                                 llvm::MaybeAlign(),
                                 llvm::AtomicOrdering::SequentiallyConsistent);
}

// The runtime pulls the current actor from a thread-local and no-ops
// without one.
llvm::Value *CodeGen::emit_intrinsic_trap(const CallExprNode &node) {
  auto *reason = emit_expr(*node.args[0]);
  builder.CreateCall(module->getFunction("saga_actor_trap"), {reason});
  return llvm::Constant::getNullValue(llvm::PointerType::getUnqual(context));
}

// Linux x86_64: rax holds the number and the result (negative is -errno);
// rdi, rsi, rdx, r10, r8, r9 hold up to six arguments, read from an
// array{int} and defaulting to 0 past its end.
llvm::Value *CodeGen::emit_intrinsic_syscall(const CallExprNode &node) {
  auto *num = emit_expr(*node.args[0]);
  auto *arr = emit_expr(*node.args[1]);

  auto *arr_struct =
      llvm::StructType::getTypeByName(context, "saga_runtime_array");
  if (!arr_struct)
    arr_struct = llvm::StructType::create(
        context, {llvm::PointerType::getUnqual(context), i64_type, i64_type},
        "saga_runtime_array");
  auto *data_gep = builder.CreateStructGEP(arr_struct, arr, 0, "arr.data.ptr");
  auto *data_ptr = builder.CreateLoad(llvm::PointerType::getUnqual(context),
                                      data_gep, "arr.data");
  auto *len_gep = builder.CreateStructGEP(arr_struct, arr, 1, "arr.len.ptr");
  auto *len = builder.CreateLoad(i64_type, len_gep, "arr.len");

  auto *zero = llvm::ConstantInt::get(i64_type, 0);
  llvm::Value *syscall_args[6];
  for (int i = 0; i < 6; ++i) {
    auto *idx = llvm::ConstantInt::get(i64_type, i);
    auto *in_bounds = builder.CreateICmpSGT(len, idx, "inb");
    auto *elem_ptr = builder.CreateGEP(i64_type, data_ptr, {idx}, "elem.ptr");
    auto *elem = builder.CreateLoad(i64_type, elem_ptr, "elem");
    syscall_args[i] = builder.CreateSelect(in_bounds, elem, zero, "arg");
  }

  auto *fn_type = llvm::FunctionType::get(
      i64_type,
      {i64_type, i64_type, i64_type, i64_type, i64_type, i64_type, i64_type},
      false);
  auto *ia = llvm::InlineAsm::get(
      fn_type, "syscall",
      "={rax},{rax},{rdi},{rsi},{rdx},{r10},{r8},{r9},~{rcx},~{r11},~{memory}",
      /*hasSideEffects=*/true);
  return builder.CreateCall(
      ia, {num, syscall_args[0], syscall_args[1], syscall_args[2],
           syscall_args[3], syscall_args[4], syscall_args[5]});
}

// The argument is `string | array{byte}`; either way the payload is a
// runtime buffer whose first field is the data pointer.
llvm::Value *CodeGen::emit_intrinsic_ptr(const CallExprNode &node) {
  auto *val = emit_expr(*node.args[0]);
  auto arg_sem = semantic_type(*node.args[0]);
  llvm::Value *str_ptr = val;
  if (arg_sem && arg_sem->kind == TypeKind::Union) {
    auto *union_st = get_union_llvm_type(arg_sem);
    auto *payload = builder.CreateStructGEP(union_st, val, 1, "ptr.payload");
    str_ptr = builder.CreateLoad(llvm::PointerType::getUnqual(context),
                                 payload, "ptr.str");
  }
  auto *data_ptr =
      builder.CreateStructGEP(string_type, str_ptr, 0, "str.data.ptr");
  auto *ptr = builder.CreateLoad(llvm::PointerType::getUnqual(context),
                                 data_ptr, "str.data");
  return builder.CreatePtrToInt(ptr, i64_type, "ptr.int");
}

} // namespace saga

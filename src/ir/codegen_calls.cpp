// Copyright 2026 Rob Thornton
// SPDX-License-Identifier: MIT

#include "ir/codegen.hpp"

#include <algorithm>
#include <llvm/IR/Constants.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/InlineAsm.h>
#include <llvm/IR/Verifier.h>

namespace saga {

// Parse an integer literal (decimal only; for intrinsic argument indices).
static int64_t parse_int_literal(std::string_view lit) {
  int64_t val = 0;
  for (char c : lit)
    if (c != '_')
      val = val * 10 + (c - '0');
  return val;
}


// ===========================================================================
// Call expressions
// ===========================================================================

llvm::Value *CodeGen::emit_call_expr(const CallExprNode &node,
                                     const Node &parent) {
  // Check for method calls on objects (selector calls like arr.Size()).
  if (std::holds_alternative<SelectorNode>(node.callee->data))
    return emit_method_or_module_call(node, parent);


  // Direct function call.
  auto *ident = std::get_if<IdentifierNode>(&node.callee->data);
  if (!ident)
    return nullptr;

  std::string name(ident->name);

  // ── Concurrency intrinsics ──────────────────────────────────────────
  // These need special handling because they inject the current_actor
  // pointer or emit inline LLVM instructions.

  // ── New stdlib intrinsics ────────────────────────────────────────────

  if (name == "intrinsic_sitofp") {
    // intrinsic_sitofp(value: Int) -> Float (f64)
    auto *val = emit_expr(*node.args[0]);
    if (!val) return nullptr;
    return builder.CreateSIToFP(val, f64_type, "sitofp");
  }

  if (name == "intrinsic_sitofp32") {
    // intrinsic_sitofp32(value: Int) -> Float32 (f32)
    auto *val = emit_expr(*node.args[0]);
    if (!val) return nullptr;
    return builder.CreateSIToFP(val, llvm::Type::getFloatTy(context), "sitofp32");
  }

  if (name == "intrinsic_fptrunc") {
    // intrinsic_fptrunc(value: Float) -> Float32 (f64 → f32)
    auto *val = emit_expr(*node.args[0]);
    if (!val) return nullptr;
    return builder.CreateFPTrunc(val, llvm::Type::getFloatTy(context), "fptrunc");
  }

  if (name == "intrinsic_fpext") {
    // intrinsic_fpext(value: Float) -> Float64 (f32 → f64)
    // On 64-bit targets this is a no-op since Float is already f64.
    auto *val = emit_expr(*node.args[0]);
    if (!val) return nullptr;
    auto *dst = llvm::Type::getDoubleTy(context);
    if (val->getType() == dst) return val; // already f64 — identity
    return builder.CreateFPExt(val, dst, "fpext");
  }

  if (name == "intrinsic_fptosi") {
    // intrinsic_fptosi(value: Float) -> Int
    auto *val = emit_expr(*node.args[0]);
    if (!val) return nullptr;
    return builder.CreateFPToSI(val, i64_type, "fptosi");
  }

  if (name == "intrinsic_zext") {
    // intrinsic_zext(value: Int, bits: Int) -> Int (i64-stored)
    // Truncate the input to `bits`, then zero-extend back to i64 so the
    // result matches the runtime's uniform i64 storage for narrow Ints.
    auto *val = emit_expr(*node.args[0]);
    if (!val) return nullptr;
    auto *bits_node = std::get_if<IntegerLiteralNode>(&node.args[1]->data);
    if (!bits_node) return nullptr;
    int64_t bits = parse_int_literal(bits_node->literal);
    if (bits >= 64) return val;
    auto *narrow_ty = llvm::IntegerType::get(context, static_cast<unsigned>(bits));
    auto *narrow = builder.CreateTrunc(val, narrow_ty, "ztrunc");
    return builder.CreateZExt(narrow, i64_type, "zext");
  }

  if (name == "intrinsic_sext") {
    // intrinsic_sext(value: Int, bits: Int) -> Int (i64-stored)
    // Truncate to `bits`, then sign-extend back to i64.  Mirrors zext
    // above; see the runtime-ABI note in CodeGen::llvm_type.
    auto *val = emit_expr(*node.args[0]);
    if (!val) return nullptr;
    auto *bits_node = std::get_if<IntegerLiteralNode>(&node.args[1]->data);
    if (!bits_node) return nullptr;
    int64_t bits = parse_int_literal(bits_node->literal);
    if (bits >= 64) return val;
    auto *narrow_ty = llvm::IntegerType::get(context, static_cast<unsigned>(bits));
    auto *narrow = builder.CreateTrunc(val, narrow_ty, "strunc");
    return builder.CreateSExt(narrow, i64_type, "sext");
  }

  if (name == "intrinsic_is_string") {
    // intrinsic_is_string(value: Any) -> Bool
    // Compile-time predicate: the argument's static type (after
    // monomorphisation, via semantic_type) folds to a constant i1.
    // LLVM constant-folds the surrounding branch, so the dead arm is
    // dropped during -O1.
    if (node.args.empty()) return nullptr;
    // Emit the argument so any side-effects in the expression still run
    // (none expected, but we don't speculate).
    (void)emit_expr(*node.args[0]);
    auto arg_sem = semantic_type(*node.args[0]);
    bool is_string = arg_sem && arg_sem->kind == TypeKind::String;
    return llvm::ConstantInt::get(i1_type, is_string ? 1 : 0);
  }

  if (name == "intrinsic_field") {
    // intrinsic_field(value: Any, index: Int) -> Any
    // Second arg must be an integer literal (field index).
    auto *val = emit_expr(*node.args[0]);
    if (!val) return nullptr;
    auto *idx_node = std::get_if<IntegerLiteralNode>(&node.args[1]->data);
    if (!idx_node) return nullptr;
    unsigned field_idx = static_cast<unsigned>(parse_int_literal(idx_node->literal));

    // The value must be a pointer; determine the backing struct type.
    // For String (saga_runtime_string*), use the string_type struct.
    auto arg_sem = semantic_type(*node.args[0]);
    llvm::StructType *backing_st = nullptr;
    if (arg_sem && arg_sem->kind == TypeKind::String) {
      backing_st = string_type;
    } else {
      // Try to find a struct type matching the value's pointee.
      // Walk struct_types looking for a match.
      for (auto &[sname, st] : struct_types) {
        (void)sname;
        if (val->getType() == llvm::PointerType::getUnqual(context)) {
          backing_st = st;
          break;
        }
      }
    }
    if (!backing_st) return nullptr;

    auto *gep = builder.CreateStructGEP(backing_st, val, field_idx, "field.ptr");
    // Return type: load the field. We return i64 for most fields.
    auto *field_type = backing_st->getElementType(field_idx);
    return builder.CreateLoad(field_type, gep, "field.val");
  }

  if (name == "intrinsic_runtime") {
    // intrinsic_runtime("func_name", args...) -> result
    // First arg must be a plain string literal naming a declared C runtime
    // function.  Extract the name from the first fragment of the string.
    if (node.args.empty()) return nullptr;
    auto *name_node = std::get_if<StringLiteralNode>(&node.args[0]->data);
    if (!name_node || name_node->fragments.empty()) return nullptr;
    auto *frag = std::get_if<StringFragmentNode>(&name_node->fragments[0]->data);
    if (!frag) return nullptr;
    // Strip surrounding quotes from the raw fragment text.
    std::string_view raw = frag->text;
    if (raw.size() >= 2 && raw.front() == '"' && raw.back() == '"')
      raw = raw.substr(1, raw.size() - 2);
    std::string func_name(raw);
    auto *callee = module->getFunction(func_name);
    if (!callee) {
      // The function isn't in the module — this is a compile-time error.
      // Return null to signal failure; semantic analysis gates this.
      return nullptr;
    }

    // Build argument list: skip args[0] (the name literal), emit the rest.
    std::vector<llvm::Value *> args;
    auto *fn_type = callee->getFunctionType();
    for (size_t i = 1; i < node.args.size(); ++i) {
      auto *val = emit_expr(*node.args[i]);
      if (!val) continue;

      // Auto-promote scalar to pointer if the C param expects a pointer.
      size_t param_idx = i - 1;
      if (param_idx < fn_type->getNumParams()) {
        auto *expected = fn_type->getParamType(param_idx);
        if (expected->isPointerTy() && !val->getType()->isPointerTy()) {
          auto *func = builder.GetInsertBlock()->getParent();
          auto *tmp = create_entry_alloca(func, "rt.tmp", val->getType());
          builder.CreateStore(val, tmp);
          val = tmp;
        }
      }
      args.push_back(val);
    }

    auto *ret_type = fn_type->getReturnType();
    if (ret_type->isVoidTy()) {
      builder.CreateCall(callee, args);
      return nullptr;
    }
    return builder.CreateCall(callee, args, "rt.call");
  }

  if (name == "intrinsic_runtime_try") {
    // intrinsic_runtime_try("func_name", args...) -> union
    // Calls a C function that writes its result to an auto-appended out-param
    // and returns i64 status (0 = success, non-zero = failure).
    // On success, wraps the out-param value as the success variant.
    // On failure, wraps Missing as the error/missing variant.
    if (node.args.empty()) return nullptr;
    auto *name_node = std::get_if<StringLiteralNode>(&node.args[0]->data);
    if (!name_node || name_node->fragments.empty()) return nullptr;
    auto *frag = std::get_if<StringFragmentNode>(&name_node->fragments[0]->data);
    if (!frag) return nullptr;
    std::string_view raw = frag->text;
    if (raw.size() >= 2 && raw.front() == '"' && raw.back() == '"')
      raw = raw.substr(1, raw.size() - 2);
    std::string func_name(raw);
    auto *callee = module->getFunction(func_name);
    if (!callee) return nullptr;

    // The enclosing function's semantic return type is the union we produce.
    auto union_sem = current_func_return_sem;
    if (!union_sem || union_sem->kind != TypeKind::Union) return nullptr;

    // Determine the success type (non-error/non-missing alternative).
    auto success_sem = strip_error_from_union(union_sem);
    if (!success_sem) return nullptr;
    auto *success_ll = llvm_type(success_sem);

    // Build argument list (same as intrinsic_runtime).
    std::vector<llvm::Value *> args;
    auto *fn_type = callee->getFunctionType();
    for (size_t i = 1; i < node.args.size(); ++i) {
      auto *val = emit_expr(*node.args[i]);
      if (!val) continue;
      size_t param_idx = i - 1;
      if (param_idx < fn_type->getNumParams()) {
        auto *expected = fn_type->getParamType(param_idx);
        if (expected->isPointerTy() && !val->getType()->isPointerTy()) {
          auto *parent_fn = builder.GetInsertBlock()->getParent();
          auto *tmp = create_entry_alloca(parent_fn, "rt.tmp", val->getType());
          builder.CreateStore(val, tmp);
          val = tmp;
        }
      }
      args.push_back(val);
    }

    // Create out-param alloca and append to args.
    auto *parent_fn = builder.GetInsertBlock()->getParent();
    auto *out_alloca = create_entry_alloca(parent_fn, "try.out", success_ll);
    builder.CreateStore(llvm::Constant::getNullValue(success_ll), out_alloca);
    args.push_back(out_alloca);

    // Call: returns i64 status.
    auto *status = builder.CreateCall(callee, args, "try.status");

    // Branch on status == 0.
    auto *is_ok = builder.CreateICmpEQ(
        status, llvm::ConstantInt::get(i64_type, 0), "try.ok");
    auto *bb_success = llvm::BasicBlock::Create(context, "try.success", parent_fn);
    auto *bb_fail = llvm::BasicBlock::Create(context, "try.fail", parent_fn);
    auto *bb_merge = llvm::BasicBlock::Create(context, "try.merge", parent_fn);
    builder.CreateCondBr(is_ok, bb_success, bb_fail);

    // Success path: load result, wrap in union.
    builder.SetInsertPoint(bb_success);
    auto *result_val = builder.CreateLoad(success_ll, out_alloca, "try.val");
    auto *wrapped_ok = emit_union_wrap(result_val, success_sem, union_sem);
    auto *bb_success_end = builder.GetInsertBlock(); // may have changed
    builder.CreateBr(bb_merge);

    // Failure path: wrap Missing in union.
    builder.SetInsertPoint(bb_fail);
    auto missing_sem = analyzer.builtins.missing_type;
    auto *missing_val = llvm::Constant::getNullValue(
        llvm::StructType::get(context)); // Missing is empty struct
    auto *wrapped_err = emit_union_wrap(missing_val, missing_sem, union_sem);
    auto *bb_fail_end = builder.GetInsertBlock();
    builder.CreateBr(bb_merge);

    // Merge with PHI.
    builder.SetInsertPoint(bb_merge);
    auto *union_st = get_union_llvm_type(union_sem);
    auto *phi = builder.CreatePHI(llvm::PointerType::getUnqual(context), 2, "try.result");
    phi->addIncoming(wrapped_ok, bb_success_end);
    phi->addIncoming(wrapped_err, bb_fail_end);
    return phi;
  }

  if (name == "intrinsic_yield") {
    // intrinsic_yield() → saga_actor_yield()
    // The runtime reads the current actor from a thread-local so this
    // call is safe from any depth inside a spawned execution context.
    // Outside an actor context the runtime itself no-ops on NULL.
    builder.CreateCall(module->getFunction("saga_actor_yield"), {});
    return llvm::Constant::getNullValue(
        llvm::PointerType::getUnqual(context));
  }

  if (name == "intrinsic_atomic_add") {
    // intrinsic_atomic_add(ptr, val) → atomicrmw add i64* %ptr, i64 %val
    // The first argument is treated as a pointer to an i64.
    // We need to get the *address* of the first argument, not its value.
    auto *val = emit_expr(*node.args[1]);
    // Get the address of the first argument (must be a variable).
    auto *ptr_ident = std::get_if<IdentifierNode>(&node.args[0]->data);
    llvm::Value *ptr = nullptr;
    if (ptr_ident) {
      auto it = locals.find(std::string(ptr_ident->name));
      if (it != locals.end())
        ptr = it->second; // alloca — this IS the pointer
    }
    if (!ptr) {
      // Fallback: emit the expression as a value (won't be atomic, but
      // won't crash).  Semantic analysis should catch misuse.
      return llvm::Constant::getNullValue(i64_type);
    }
    return builder.CreateAtomicRMW(llvm::AtomicRMWInst::Add, ptr, val,
                                   llvm::MaybeAlign(),
                                   llvm::AtomicOrdering::SequentiallyConsistent);
  }

  if (name == "intrinsic_trap") {
    // intrinsic_trap(reason) → saga_actor_trap(reason)
    // The runtime pulls the current actor from a thread-local so the
    // intrinsic works from any call depth.  If there is no current
    // actor (illegal use outside a spawn body), the runtime no-ops.
    auto *reason = emit_expr(*node.args[0]);
    builder.CreateCall(module->getFunction("saga_actor_trap"), {reason});
    return llvm::Constant::getNullValue(
        llvm::PointerType::getUnqual(context));
  }

  if (name == "intrinsic_syscall") {
    // intrinsic_syscall(num, args_array) → inline syscall instruction.
    // num is the syscall number (i64), args_array is an [Int] with up to
    // 6 elements.
    // On Linux x86_64 the syscall convention is:
    //   rax = syscall number
    //   rdi, rsi, rdx, r10, r8, r9 = arguments
    //   rax = return value (negative = -errno)
    auto *num = emit_expr(*node.args[0]);
    auto *arr = emit_expr(*node.args[1]);

    // Load up to 6 elements from the array. saga_runtime_array is { i64*, i64, i64 }
    // where field 0 = data ptr, field 1 = length.
    auto *arr_struct = llvm::StructType::getTypeByName(context, "saga_runtime_array");
    if (!arr_struct)
      arr_struct = llvm::StructType::create(
          context,
          {llvm::PointerType::getUnqual(context), i64_type, i64_type},
          "saga_runtime_array");
    auto *data_gep = builder.CreateStructGEP(arr_struct, arr, 0, "arr.data.ptr");
    auto *data_ptr = builder.CreateLoad(
        llvm::PointerType::getUnqual(context), data_gep, "arr.data");
    auto *len_gep = builder.CreateStructGEP(arr_struct, arr, 1, "arr.len.ptr");
    auto *len = builder.CreateLoad(i64_type, len_gep, "arr.len");

    // Load each argument with a bounds check, defaulting to 0.
    auto *zero = llvm::ConstantInt::get(i64_type, 0);
    llvm::Value *syscall_args[6];
    for (int i = 0; i < 6; ++i) {
      auto *idx = llvm::ConstantInt::get(i64_type, i);
      auto *in_bounds = builder.CreateICmpSGT(len, idx, "inb");
      auto *elem_ptr = builder.CreateGEP(i64_type, data_ptr, {idx}, "elem.ptr");
      auto *elem = builder.CreateLoad(i64_type, elem_ptr, "elem");
      syscall_args[i] = builder.CreateSelect(in_bounds, elem, zero, "arg");
    }

    // Build the inline asm for syscall.
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

  if (name == "intrinsic_ptr") {
    // intrinsic_ptr(value) → load the data pointer from the backing
    // buffer.  The argument is String | [Byte] (a union). We need
    // to extract the payload pointer and then load field 0 of
    // saga_runtime_string (the data pointer).
    auto *val = emit_expr(*node.args[0]);
    auto arg_sem = semantic_type(*node.args[0]);
    llvm::Value *str_ptr = val;
    // If the arg is a union, val is a ptr to the union struct alloca.
    // Extract the payload (which is the string/array pointer).
    if (arg_sem && arg_sem->kind == TypeKind::Union) {
      auto *union_st = get_union_llvm_type(arg_sem);
      auto *payload = builder.CreateStructGEP(union_st, val, 1, "ptr.payload");
      str_ptr = builder.CreateLoad(
          llvm::PointerType::getUnqual(context), payload, "ptr.str");
    }
    auto *data_ptr = builder.CreateStructGEP(string_type, str_ptr, 0, "str.data.ptr");
    auto *ptr = builder.CreateLoad(
        llvm::PointerType::getUnqual(context), data_ptr, "str.data");
    return builder.CreatePtrToInt(ptr, i64_type, "ptr.int");
  }

  // ── Generic free-function dispatch ──────────────────────────────────
  // If the callee is a generic free function, emit (or reuse) a
  // monomorphised specialisation and call it directly.
  {
    auto callee_sem = semantic_type(*node.callee);
    if (callee_sem && callee_sem->kind == TypeKind::Func) {
      auto fd_it = analyzer.func_decl_by_type_.find(callee_sem.get());
      if (fd_it != analyzer.func_decl_by_type_.end()) {
        const FuncDeclNode *fn_decl = fd_it->second;
        if (fn_decl->generic && !fn_decl->receiver) {
          auto *ta_ptr = node_type_args_of(parent);
          if (ta_ptr) {
            auto &bindings = *ta_ptr;

            // Find the matching BodyInstantiation.
            const Analyzer::BodyInstantiation *inst = nullptr;
            auto inst_it = analyzer.instantiations_.find(fn_decl);
            if (inst_it != analyzer.instantiations_.end()) {
              for (auto &i : inst_it->second) {
                if (i.bindings.size() == bindings.size()) {
                  bool match = true;
                  for (auto &[id, t] : bindings) {
                    auto j = i.bindings.find(id);
                    if (j == i.bindings.end() ||
                        !types_equal(t, j->second)) {
                      match = false;
                      break;
                    }
                  }
                  if (match) { inst = &i; break; }
                }
              }
            }

            auto *spec = emit_specialisation(*fn_decl, callee_sem,
                                             bindings, inst);
            if (spec) {
              std::vector<llvm::Value *> args;
              for (auto &a : node.args) {
                auto *v = emit_expr(*a);
                if (v) args.push_back(v);
              }
              if (spec->getReturnType()->isVoidTy()) {
                builder.CreateCall(spec, args);
                return nullptr;
              }
              return builder.CreateCall(spec, args, "gen.call");
            }
          }
        }
      }
    }
  }

  // ── Regular function dispatch ───────────────────────────────────────
  std::string link_name;
  if (name == "intrinsic_print")
    link_name = "saga_intrinsic_print";
  else
    link_name = mangle(name);

  auto *callee = module->getFunction(link_name);

  // If not a known module function, check if it's a function-typed local
  // or parameter (first-class function value).  v1: free functions only —
  // the value is a raw LLVM function pointer; no env, no closure struct.
  if (!callee) {
    auto local_it = locals.find(name);
    if (local_it != locals.end()) {
      auto *alloca = local_it->second;
      auto callee_sem = semantic_type(*node.callee);
      bool is_func_typed = callee_sem && callee_sem->kind == TypeKind::Func;
      if (is_func_typed) {
        auto *ptr_type = llvm::PointerType::getUnqual(context);
        bool is_closure =
            alloca->getAllocatedType() == closure_fat_ptr_type;

        // Closure value carries (fn, env); plain function value is just fn.
        // The trampoline expects env as its first arg.
        llvm::Value *fn_ptr = nullptr;
        llvm::Value *env_ptr = nullptr;
        if (is_closure) {
          auto *fn_gep = builder.CreateStructGEP(
              closure_fat_ptr_type, alloca, 0, "closure.fn.gep");
          fn_ptr = builder.CreateLoad(ptr_type, fn_gep, "closure.fn");
          auto *env_gep = builder.CreateStructGEP(
              closure_fat_ptr_type, alloca, 1, "closure.env.gep");
          env_ptr = builder.CreateLoad(ptr_type, env_gep, "closure.env");
        } else {
          fn_ptr = builder.CreateLoad(ptr_type, alloca, "fn.load");
        }

        std::vector<llvm::Value *> args;
        std::vector<llvm::Type *> param_types;
        if (is_closure) {
          args.push_back(env_ptr);
          param_types.push_back(ptr_type);
        }
        for (auto &arg_node : node.args) {
          auto *val = emit_expr(*arg_node);
          if (val)
            args.push_back(val);
        }

        llvm::Type *ret_ll = void_ll_type;
        auto &fi = std::get<FuncTypeInfo>(callee_sem->detail);
        for (auto &pt : fi.params)
          param_types.push_back(llvm_type(pt));
        if (!fi.returns.empty())
          ret_ll = llvm_type(fi.returns[0]);

        auto *fn_type = llvm::FunctionType::get(ret_ll, param_types, false);
        if (ret_ll->isVoidTy()) {
          builder.CreateCall(fn_type, fn_ptr, args);
          return nullptr;
        }
        return builder.CreateCall(fn_type, fn_ptr, args, "fn.call");
      }
    }
    return nullptr;
  }

  std::vector<llvm::Value *> args;

  // Sret lowering for direct dispatch.
  auto *parent_fn = builder.GetInsertBlock()->getParent();
  llvm::Value *sret_slot = nullptr;
  llvm::Type *sret_struct_ty = nullptr;
  if (callee->arg_size() > 0 && callee->getArg(0)->hasStructRetAttr()) {
    sret_struct_ty = callee->getParamStructRetType(0);
    sret_slot = create_entry_alloca(parent_fn, "sret.tmp", sret_struct_ty);
    args.push_back(sret_slot);
  }

  // Resolve the semantic param types so we can recognise struct args
  // that need spilling for byval.
  auto callee_sem = semantic_type(*node.callee);
  const FuncTypeInfo *fi = nullptr;
  if (callee_sem && callee_sem->kind == TypeKind::Func)
    fi = &std::get<FuncTypeInfo>(callee_sem->detail);

  // Variadic call with multiple scalar args: pack them into a fresh
  // saga_runtime_array and pass the pointer as the variadic arg.
  // The array-passthrough case (single array arg matching the variadic
  // array type) is handled by the per-arg loop below.
  llvm::Value *variadic_packed = nullptr;
  size_t variadic_idx = 0;
  bool variadic_is_passthrough = false;
  if (fi && fi->is_variadic && !fi->params.empty()) {
    variadic_idx = fi->params.size() - 1;
    auto &last = fi->params.back();
    if (node.args.size() == fi->params.size() &&
        last && last->kind == TypeKind::Array) {
      auto last_arg_sem = semantic_type(*node.args.back());
      if (last_arg_sem && types_equal(last_arg_sem, last))
        variadic_is_passthrough = true;
    }
    if (!variadic_is_passthrough &&
        last && last->kind == TypeKind::Array) {
      auto &arr = std::get<ArrayTypeInfo>(last->detail);
      auto *elem_ll = llvm_type(arr.element);
      uint64_t elem_size =
          elem_ll ? module->getDataLayout().getTypeAllocSize(elem_ll)
                  : 8;
      auto *new_fn = module->getFunction("saga_array_new");
      auto *push_fn = module->getFunction("saga_array_push");
      int64_t var_count = node.args.size() > variadic_idx
                              ? static_cast<int64_t>(node.args.size() -
                                                     variadic_idx)
                              : 0;
      std::vector<llvm::Value *> new_args = {
          llvm::ConstantInt::get(i64_type, elem_size),
          llvm::ConstantInt::get(i64_type,
                                 std::max<int64_t>(var_count, 4))};
      auto *arr_val = builder.CreateCall(new_fn, new_args, "var.arr");
      for (size_t i = variadic_idx; i < node.args.size(); ++i) {
        auto *val = emit_expr(*node.args[i]);
        if (!val) continue;
        auto *tmp =
            create_entry_alloca(parent_fn, "var.tmp", val->getType());
        builder.CreateStore(val, tmp);
        std::vector<llvm::Value *> push_args = {arr_val, tmp};
        builder.CreateCall(push_fn, push_args);
      }
      variadic_packed = arr_val;
    }
  }

  for (size_t i = 0; i < node.args.size(); ++i) {
    if (variadic_packed && i >= variadic_idx) {
      args.push_back(variadic_packed);
      break;
    }
    auto *val = emit_expr(*node.args[i]);
    if (!val)
      continue;
    // Spec docs/language.md:51 — values that escape their scope are
    // copied. The function-call boundary is an escape, so deep-copy
    // arrays before passing so the callee operates on its own copy.
    auto arg_sem = semantic_type(*node.args[i]);
    if (arg_sem && arg_sem->kind == TypeKind::Array) {
      val = builder.CreateCall(module->getFunction("saga_array_clone"),
                               {val}, "arg.clone");
    }
    // Interface boxing: param expects an interface, arg is a concrete
    // struct.  Spill struct SSA values, then wrap the struct pointer
    // in a fat pointer { data, vtable }.
    if (fi && i < fi->params.size() && fi->params[i] &&
        fi->params[i]->kind == TypeKind::Interface) {
      auto arg_sem = semantic_type(*node.args[i]);
      if (arg_sem && arg_sem->kind == TypeKind::Struct) {
        llvm::Value *struct_ptr = val;
        if (val->getType()->isStructTy()) {
          auto *p_ll = llvm_type(arg_sem);
          auto *tmp = create_entry_alloca(parent_fn, "iface.arg.spill", p_ll);
          builder.CreateStore(val, tmp);
          struct_ptr = tmp;
        }
        auto *boxed = emit_interface_box(struct_ptr, arg_sem, fi->params[i]);
        if (boxed)
          val = boxed;
      }
    }
    // Wrap a non-union arg into the union when the param expects one
    // (`f Int|Float = 7`).  Without this, the byval attribute attaches
    // to the raw scalar value and the callee's memcpy reads through
    // address `7` → segfault.
    if (fi && i < fi->params.size() && fi->params[i] &&
        fi->params[i]->kind == TypeKind::Union) {
      auto arg_sem = semantic_type(*node.args[i]);
      if (arg_sem && arg_sem->kind != TypeKind::Union) {
        auto *wrapped =
            emit_union_wrap(val, arg_sem, fi->params[i]);
        if (wrapped) val = wrapped;
      }
    }
    if (fi && i < fi->params.size() && fi->params[i] &&
        (fi->params[i]->kind == TypeKind::Struct ||
         fi->params[i]->kind == TypeKind::Union)) {
      auto *p_ll = llvm_type(fi->params[i]);
      if (p_ll && p_ll->isStructTy() && val->getType()->isStructTy()) {
        auto *tmp = create_entry_alloca(parent_fn, "arg.spill", p_ll);
        builder.CreateStore(val, tmp);
        val = tmp;
      }
    }
    args.push_back(val);
  }

  auto *call = builder.CreateCall(callee, args);

  // Mirror sret/byval attrs on the call site.
  unsigned cidx = 0;
  if (sret_slot) {
    call->addParamAttr(cidx,
        llvm::Attribute::getWithStructRetType(context, sret_struct_ty));
    call->addParamAttr(cidx,
        llvm::Attribute::getWithAlignment(context,
            module->getDataLayout().getABITypeAlign(sret_struct_ty)));
    ++cidx;
  }
  if (fi) {
    for (size_t i = 0; i < fi->params.size(); ++i) {
      if (fi->params[i] &&
          (fi->params[i]->kind == TypeKind::Struct ||
           fi->params[i]->kind == TypeKind::Union)) {
        auto *p_ll = llvm_type(fi->params[i]);
        if (p_ll && p_ll->isStructTy()) {
          call->addParamAttr(cidx,
              llvm::Attribute::getWithByValType(context, p_ll));
          call->addParamAttr(cidx,
              llvm::Attribute::getWithAlignment(context,
                  module->getDataLayout().getABITypeAlign(p_ll)));
        }
      }
      ++cidx;
    }
  }

  if (sret_slot)
    return sret_slot;
  return call;
}

// ===========================================================================
// Identifier expressions
// ===========================================================================

llvm::Value *CodeGen::emit_identifier(const IdentifierNode &node) {
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

// Copyright 2026 Rob Thornton
// SPDX-License-Identifier: MIT

#include "ir/codegen.hpp"

#include <algorithm>
#include <llvm/IR/Constants.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/InlineAsm.h>
#include <llvm/IR/Verifier.h>

namespace mc {

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
    // intrinsic_zext(value: Int, bits: Int) -> Int
    // bits must be an integer literal.
    auto *val = emit_expr(*node.args[0]);
    if (!val) return nullptr;
    auto *bits_node = std::get_if<IntegerLiteralNode>(&node.args[1]->data);
    if (!bits_node) return nullptr;
    int64_t bits = parse_int_literal(bits_node->literal);
    auto *dst_type = llvm::IntegerType::get(context, static_cast<unsigned>(bits));
    unsigned src_bits = val->getType()->getIntegerBitWidth();
    if (bits < static_cast<int64_t>(src_bits))
      return builder.CreateTrunc(val, dst_type, "ztrunc");
    if (bits > static_cast<int64_t>(src_bits))
      return builder.CreateZExt(val, dst_type, "zext");
    return val;
  }

  if (name == "intrinsic_sext") {
    // intrinsic_sext(value: Int, bits: Int) -> Int
    auto *val = emit_expr(*node.args[0]);
    if (!val) return nullptr;
    auto *bits_node = std::get_if<IntegerLiteralNode>(&node.args[1]->data);
    if (!bits_node) return nullptr;
    int64_t bits = parse_int_literal(bits_node->literal);
    auto *dst_type = llvm::IntegerType::get(context, static_cast<unsigned>(bits));
    unsigned src_bits = val->getType()->getIntegerBitWidth();
    if (bits < static_cast<int64_t>(src_bits))
      return builder.CreateTrunc(val, dst_type, "strunc");
    if (bits > static_cast<int64_t>(src_bits))
      return builder.CreateSExt(val, dst_type, "sext");
    return val;
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
    // For String (mc_string*), use the string_type struct.
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

    // Load up to 6 elements from the array. mc_array is { i64*, i64, i64 }
    // where field 0 = data ptr, field 1 = length.
    auto *arr_struct = llvm::StructType::getTypeByName(context, "mc_array");
    if (!arr_struct)
      arr_struct = llvm::StructType::create(
          context,
          {llvm::PointerType::getUnqual(context), i64_type, i64_type},
          "mc_array");
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
    // mc_string (the data pointer).
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
        // The local's alloca holds a ptr; load it to get the fn pointer.
        auto *fn_ptr = builder.CreateLoad(ptr_type, alloca, "fn.load");

        std::vector<llvm::Value *> args;
        for (auto &arg_node : node.args) {
          auto *val = emit_expr(*arg_node);
          if (val)
            args.push_back(val);
        }

        std::vector<llvm::Type *> param_types;
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

  for (size_t i = 0; i < node.args.size(); ++i) {
    auto *val = emit_expr(*node.args[i]);
    if (!val)
      continue;
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

// ===========================================================================
// Function expressions (closures)
// ===========================================================================

llvm::Value *CodeGen::emit_func_expr(const FuncExprNode &node,
                                      const Node &parent) {
  auto *ptr_type = llvm::PointerType::getUnqual(context);
  auto *enclosing_func = builder.GetInsertBlock()->getParent();

  // Generate a unique name for this closure.
  std::string closure_name = "mc.closure." + std::to_string(next_closure_id++);

  // ── Look up captured variables ─────────────────────────────────────
  std::vector<Analyzer::CaptureInfo> captures;
  if (auto *caps = node_captures_of(parent))
    captures = *caps;

  // ── Build the environment struct type ──────────────────────────────
  // The env struct holds one field per captured variable.
  std::vector<llvm::Type *> env_field_types;
  std::vector<std::string> env_field_names;
  for (auto &cap : captures) {
    auto *ll = llvm_type(cap.type);
    env_field_types.push_back(ll);
    env_field_names.push_back(cap.name);
  }

  llvm::StructType *env_type = nullptr;
  if (!env_field_types.empty()) {
    env_type = llvm::StructType::create(context, env_field_types,
                                         closure_name + ".env");
  }

  // ── Build the trampoline function ──────────────────────────────────
  // Signature: ret_type trampoline(ptr env, param1, param2, ...)
  std::vector<llvm::Type *> tramp_param_types;
  tramp_param_types.push_back(ptr_type); // env pointer (always first)

  for (auto &param : node.signature.params) {
    auto *ll = resolve_type_node(*param.type);
    for (size_t i = 0; i < param.names.identifiers.size(); ++i)
      tramp_param_types.push_back(ll);
  }

  llvm::Type *ret_type = void_ll_type;
  if (!node.signature.returns.empty())
    ret_type = resolve_type_node(*node.signature.returns[0]);

  auto *tramp_fn_type = llvm::FunctionType::get(ret_type, tramp_param_types,
                                                  false);
  auto *tramp_fn = llvm::Function::Create(
      tramp_fn_type, llvm::Function::InternalLinkage, closure_name,
      module.get());

  // Name the arguments.
  tramp_fn->getArg(0)->setName("env");
  size_t arg_idx = 1;
  for (auto &param : node.signature.params)
    for (auto &ident : param.names.identifiers)
      if (arg_idx < tramp_fn->arg_size())
        tramp_fn->getArg(arg_idx++)->setName(std::string(ident.name));

  // ── Emit the trampoline body ───────────────────────────────────────
  // Save current insertion state.
  auto saved_block = builder.GetInsertBlock();
  auto saved_point = builder.GetInsertPoint();
  auto saved_locals = locals;
  auto saved_managed = managed_locals;
  auto saved_is_main = current_func_is_main;

  auto *entry = llvm::BasicBlock::Create(context, "entry", tramp_fn);
  builder.SetInsertPoint(entry);

  locals.clear();
  managed_locals.clear();
  current_func_is_main = false;

  // Unpack environment struct into local variables.
  if (env_type) {
    auto *env_ptr = tramp_fn->getArg(0);
    for (size_t i = 0; i < captures.size(); ++i) {
      auto *field_gep = builder.CreateStructGEP(env_type, env_ptr, i,
                                                  captures[i].name + ".cap");
      auto *field_type = env_field_types[i];
      auto *val = builder.CreateLoad(field_type, field_gep,
                                      captures[i].name);
      auto *alloca = create_entry_alloca(tramp_fn, captures[i].name,
                                          field_type);
      builder.CreateStore(val, alloca);
      locals[captures[i].name] = alloca;
    }
  }

  // Create allocas for parameters.
  arg_idx = 1;
  for (auto &param : node.signature.params) {
    auto *ll = resolve_type_node(*param.type);
    for (auto &ident : param.names.identifiers) {
      std::string pname(ident.name);
      auto *alloca = create_entry_alloca(tramp_fn, pname, ll);
      builder.CreateStore(tramp_fn->getArg(arg_idx++), alloca);
      locals[pname] = alloca;
    }
  }

  // Emit the closure body.
  auto &block = std::get<BlockNode>(node.body->data);
  auto *tail_val = emit_block(block);

  // Add terminator if needed.
  if (!builder.GetInsertBlock()->getTerminator()) {
    emit_release_locals();
    if (ret_type->isVoidTy()) {
      builder.CreateRetVoid();
    } else if (tail_val && tail_val->getType() == ret_type) {
      builder.CreateRet(tail_val);
    } else {
      builder.CreateRet(llvm::Constant::getNullValue(ret_type));
    }
  }

  llvm::verifyFunction(*tramp_fn);

  // ── Restore the enclosing function's state ─────────────────────────
  builder.SetInsertPoint(saved_block, saved_point);
  locals = saved_locals;
  managed_locals = saved_managed;
  current_func_is_main = saved_is_main;

  // ── Allocate and populate the environment struct ───────────────────
  llvm::Value *env_ptr = llvm::ConstantPointerNull::get(ptr_type);
  if (env_type) {
    auto *env_alloca = create_entry_alloca(enclosing_func,
                                            closure_name + ".env",
                                            env_type);
    // Store each captured variable's current value into the env.
    for (size_t i = 0; i < captures.size(); ++i) {
      auto *field_gep = builder.CreateStructGEP(env_type, env_alloca, i,
                                                  captures[i].name + ".env.f");
      // Load the current value of the captured variable.
      auto local_it = locals.find(captures[i].name);
      if (local_it != locals.end()) {
        auto *alloca = local_it->second;
        auto *val = builder.CreateLoad(env_field_types[i], alloca,
                                        captures[i].name + ".cap.val");
        builder.CreateStore(val, field_gep);
      } else {
        // Zero-initialize if not found (shouldn't happen after analysis).
        builder.CreateStore(
            llvm::Constant::getNullValue(env_field_types[i]), field_gep);
      }
    }
    env_ptr = env_alloca;
  }

  // ── Build the closure fat pointer { fn_ptr, env_ptr } ─────────────
  auto *closure_alloca = create_entry_alloca(enclosing_func,
                                              closure_name + ".val",
                                              closure_fat_ptr_type);
  auto *fn_gep = builder.CreateStructGEP(closure_fat_ptr_type,
                                          closure_alloca, 0, "closure.fn");
  builder.CreateStore(tramp_fn, fn_gep);
  auto *env_gep = builder.CreateStructGEP(closure_fat_ptr_type,
                                            closure_alloca, 1, "closure.env");
  builder.CreateStore(env_ptr, env_gep);

  return closure_alloca;
}

// ===========================================================================
// Spawn expression
// ===========================================================================

llvm::Value *CodeGen::emit_spawn_expr(const SpawnExprNode &node,
                                       const Node &parent) {
  has_spawn = true;

  auto *ptr_type = llvm::PointerType::getUnqual(context);
  auto *enclosing_func = builder.GetInsertBlock()->getParent();

  // Generate a unique name for this spawn's outlined function.
  std::string spawn_name = "mc.spawn." + std::to_string(next_spawn_id++);

  // ── Look up captured variables ─────────────────────────────────────
  std::vector<Analyzer::SpawnCaptureInfo> captures;
  if (auto *caps = spawn_captures_of(parent))
    captures = *caps;

  // ── Build the closure struct type ──────────────────────────────────
  // Holds one field per captured variable.
  std::vector<llvm::Type *> closure_field_types;
  std::vector<std::string> closure_field_names;
  for (auto &cap : captures) {
    auto *ll = llvm_type(cap.type);
    closure_field_types.push_back(ll);
    closure_field_names.push_back(cap.name);
  }

  llvm::StructType *closure_st = nullptr;
  uint64_t closure_size = 0;
  if (!closure_field_types.empty()) {
    closure_st = llvm::StructType::create(context, closure_field_types,
                                           spawn_name + ".closure");
    closure_size = module->getDataLayout().getTypeAllocSize(closure_st);
  }

  // ── Build the outlined function: void(mc_actor*) ───────────────────
  auto *outlined_fn_type = llvm::FunctionType::get(
      void_ll_type, {ptr_type}, false);
  auto *outlined_fn = llvm::Function::Create(
      outlined_fn_type, llvm::Function::InternalLinkage, spawn_name,
      module.get());
  outlined_fn->getArg(0)->setName("actor");

  // ── Emit the outlined function body ────────────────────────────────
  auto saved_block = builder.GetInsertBlock();
  auto saved_point = builder.GetInsertPoint();
  auto saved_locals = locals;
  auto saved_managed = managed_locals;
  auto saved_is_main = current_func_is_main;
  auto saved_actor = current_actor;

  auto *entry = llvm::BasicBlock::Create(context, "entry", outlined_fn);
  builder.SetInsertPoint(entry);

  locals.clear();
  managed_locals.clear();
  current_func_is_main = false;
  current_actor = outlined_fn->getArg(0);

  // Unpack closure data from actor->closure_data.
  // The runtime copies the closure struct into the actor's arena, so we
  // just read it back from the pointer stored in the mc_actor.  However,
  // we don't have the C struct layout in LLVM IR, so the runtime passes
  // the closure_data pointer.  We store it in a local and GEP into the
  // closure struct type.
  //
  // The mc_actor struct has closure_data at a known offset.  We'll use
  // a dedicated runtime helper or a fixed byte offset.  Since mc_actor
  // is opaque to codegen, we pass the closure_data as the entry arg's
  // associated data.  The runtime stores it in actor->closure_data and
  // the worker loop calls entry(actor).  We load it via:
  //   closure_data = *(void**)(actor + offsetof(mc_actor, closure_data))
  //
  // offsetof(mc_actor, closure_data) needs to match the C struct.
  // mc_actor layout (64-bit): refcount(8) result(8) result_size(8)
  //   status(8) cancelled(8) lock(40) done_cond(48) arena(8)
  //   entry(8) closure_data(8) ...
  // That's fragile.  Instead, generate a tiny C helper.
  //
  // Simpler approach: add a runtime function mc_actor_closure_data(actor)
  // that returns actor->closure_data.
  //
  // For now, let's define the offset based on the mc_actor C struct.
  // A more robust approach: declare mc_actor_closure_data as a runtime fn.
  //
  // Actually, the cleanest approach: we already have the closure_data
  // pointer passed via mc_actor_new.  The outlined function receives the
  // actor*, and the closure_data was memcpy'd into the arena.
  // We need to access it.  Let's add a trivial runtime accessor.

  if (closure_st && !captures.empty()) {
    // Declare or get the accessor: void* mc_actor_get_closure(mc_actor*)
    auto *accessor = module->getFunction("mc_actor_get_closure");
    if (!accessor) {
      accessor = llvm::Function::Create(
          llvm::FunctionType::get(ptr_type, {ptr_type}, false),
          llvm::Function::ExternalLinkage, "mc_actor_get_closure",
          module.get());
    }

    auto *closure_ptr = builder.CreateCall(accessor, {current_actor},
                                            "closure.ptr");

    for (size_t i = 0; i < captures.size(); ++i) {
      auto *field_gep = builder.CreateStructGEP(
          closure_st, closure_ptr, i, captures[i].name + ".cap");
      auto *val = builder.CreateLoad(closure_field_types[i], field_gep,
                                      captures[i].name);
      auto *alloca = create_entry_alloca(outlined_fn, captures[i].name,
                                          closure_field_types[i]);
      builder.CreateStore(val, alloca);
      locals[captures[i].name] = alloca;

      // Retain shared refcounted captures.
      if (captures[i].kind == Analyzer::SpawnCaptureKind::Share) {
        // The runtime already memcpy'd the data, but we need to retain
        // the refcounted objects so they aren't freed while we use them.
        TypePtr cap_type = captures[i].type;
        emit_retain(val, cap_type);
      }
    }
  }

  // If the spawn has a pipe variable (|task|), bind it to the actor ptr.
  if (node.pipe) {
    std::string pipe_name(node.pipe->name);
    auto *alloca = create_entry_alloca(outlined_fn, pipe_name, ptr_type);
    builder.CreateStore(current_actor, alloca);
    locals[pipe_name] = alloca;
  }

  // Emit the spawn body.
  if (auto *block = std::get_if<BlockNode>(&node.body->data)) {
    emit_block(*block);
  } else {
    // Identifier body (function reference) — call it as a trampoline.
    auto *body_val = emit_expr(*node.body);
    // If it returned a value, we could use context_exit, but for
    // simplicity we just ignore it; the function should use task.Exit().
    (void)body_val;
  }

  // Add terminator if needed.
  if (!builder.GetInsertBlock()->getTerminator())
    builder.CreateRetVoid();

  llvm::verifyFunction(*outlined_fn);

  // ── Restore enclosing function state ───────────────────────────────
  builder.SetInsertPoint(saved_block, saved_point);
  locals = saved_locals;
  managed_locals = saved_managed;
  current_func_is_main = saved_is_main;
  current_actor = saved_actor;

  // ── Pack closure data at the spawn site ────────────────────────────
  llvm::Value *closure_ptr_val =
      llvm::ConstantPointerNull::get(ptr_type);

  if (closure_st && !captures.empty()) {
    auto *closure_alloca = create_entry_alloca(
        enclosing_func, spawn_name + ".closure", closure_st);

    for (size_t i = 0; i < captures.size(); ++i) {
      auto *field_gep = builder.CreateStructGEP(
          closure_st, closure_alloca, i, captures[i].name + ".pack");

      auto local_it = locals.find(captures[i].name);
      if (local_it != locals.end()) {
        auto *alloca = local_it->second;
        auto *val = builder.CreateLoad(closure_field_types[i], alloca,
                                        captures[i].name + ".cap.val");
        builder.CreateStore(val, field_gep);

        // Retain shared refcounted captures at the spawn site.
        if (captures[i].kind == Analyzer::SpawnCaptureKind::Share)
          emit_retain(val, captures[i].type);
      } else {
        builder.CreateStore(
            llvm::Constant::getNullValue(closure_field_types[i]),
            field_gep);
      }
    }
    closure_ptr_val = closure_alloca;
  }

  // ── Create channel if generic type present ─────────────────────────
  llvm::Value *channel_ptr = nullptr;
  int64_t channel_elem_size = 0;
  if (node.generic && !node.generic->type_params.empty()) {
    // Read the channel element type the analyzer resolved while the
    // spawn's enclosing scope was still live.  Re-resolving here would
    // fail for user struct names because their scope has been popped.
    TypePtr chan_elem_sem = spawn_channel_elem_type_of(parent);
    if (!chan_elem_sem)
      chan_elem_sem = analyzer.resolve_type(*node.generic->type_params[0]);
    auto *chan_elem_ll = llvm_type(chan_elem_sem);
    if (!chan_elem_ll || chan_elem_ll->isVoidTy())
      chan_elem_ll = i64_type; // defensive fallback; shouldn't happen
    channel_elem_size = static_cast<int64_t>(
        module->getDataLayout().getTypeAllocSize(chan_elem_ll));

    auto *ch = builder.CreateCall(
        module->getFunction("saga_channel_new"),
        {llvm::ConstantInt::get(i64_type, channel_elem_size),
         llvm::ConstantInt::get(i64_type, 0)}, // 0 = default capacity
        "channel");
    channel_ptr = ch;
  }

  // ── Spawn the actor ────────────────────────────────────────────────
  auto *actor = builder.CreateCall(
      module->getFunction("saga_executor_spawn"),
      {outlined_fn,
       closure_ptr_val,
       llvm::ConstantInt::get(i64_type, static_cast<int64_t>(closure_size)),
       llvm::ConstantInt::get(i64_type, 0)}, // 0 = default arena max
      "actor");

  // Attach channel to actor if present.
  // We need to store ch into actor->channel.  Use a runtime helper.
  if (channel_ptr) {
    auto *attach_fn = module->getFunction("mc_actor_set_channel");
    if (!attach_fn) {
      attach_fn = llvm::Function::Create(
          llvm::FunctionType::get(void_ll_type, {ptr_type, ptr_type}, false),
          llvm::Function::ExternalLinkage, "mc_actor_set_channel",
          module.get());
    }
    builder.CreateCall(attach_fn, {actor, channel_ptr});
  }

  // ── Store the actor ptr as the Task handle ─────────────────────────
  // The actor ptr IS the Task — mc_actor* is our runtime Task handle.
  // Store it in a local alloca (caller will bind it via DeclAssign).

  // If there's a channel, also store it as a companion local so the
  // for-range iteration can find it.  The DeclAssign handler will bind
  // the task name, and we store the channel under "<name>.channel".
  // Since we don't know the target name yet, we use a temporary.
  // The channel is returned alongside the actor via a small wrapper.

  // For channel-based spawns, store the channel ptr in a pseudo-local
  // that the for-range handler can look up.
  if (channel_ptr) {
    // We'll store it with a predictable name.  The DeclAssign that
    // receives this spawn result will rename the actor alloca.  We
    // stash the channel in a local keyed by the spawn id.
    std::string ch_local = spawn_name + ".channel";
    auto *ch_alloca = create_entry_alloca(enclosing_func, ch_local, ptr_type);
    builder.CreateStore(channel_ptr, ch_alloca);
    locals[ch_local] = ch_alloca;

    // Also, we'll need to make the DeclAssign handler aware.
    // Store a mapping from the actor value to the channel local name.
    // For simplicity, we use a side-channel: after the DeclAssign binds
    // the task name, we create a "<taskname>.channel" alias.
    // We handle this by storing the channel under a known key and
    // having a post-decl-assign hook.  Instead, let's just store the
    // channel alloca indexed by the actor pointer value — but that's
    // fragile with LLVM values.
    //
    // Simplest approach: return a tagged pair.  But since our codegen
    // returns a single llvm::Value*, we need another mechanism.
    //
    // Pragmatic solution: stash the pending channel in a member variable.
    // The next DeclAssign that receives this actor ptr will pick it up.
    pending_channel_alloca_ = ch_alloca;
  }

  return actor;
}

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
  // Keys are qualified (e.g. "test__Colors"), so fall back via key_for.
  if (enum_types.count(key_for("", name)))
    return llvm::ConstantInt::get(i64_type, 0);

  // Top-level function referenced as a value (e.g. `call_it(greet, ...)` or
  // a struct literal like `Reg{ handler: greet }`).  Return the raw LLVM
  // Function* — it is pointer-typed, matching how function-typed locals
  // and struct fields are lowered.
  if (auto *fn = module->getFunction(mangle(name)))
    return fn;

  return nullptr;
}

} // namespace mc

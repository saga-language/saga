// Copyright 2026 Rob Thornton
// SPDX-License-Identifier: MIT

// Methods on an enum value. An enum is its i64 ordinal in IR, so Int() is the
// value itself; String() and From() are generated per enum.

#include "ir/codegen.hpp"

#include <llvm/IR/Constants.h>

namespace saga {

llvm::Value *CodeGen::emit_enum_method_call(const CallExprNode &node,
                                            const std::string &method,
                                            const TypePtr &enum_sem,
                                            llvm::Value *obj) {
  if (method == "Int")
    return obj;
  if (method == "String")
    return builder.CreateCall(enum_string_fn(enum_sem), {obj}, "enum.str");
  if (method == "From")
    return emit_enum_from(node, enum_sem);
  return emit_enum_user_method_call(node, method, enum_sem, obj);
}

// Int, String and From are reserved from redefinition, so a method reaching
// here is one the enum's own package declared, with the ordinal as self.
llvm::Value *CodeGen::emit_enum_user_method_call(const CallExprNode &node,
                                                 const std::string &method,
                                                 const TypePtr &obj_sem,
                                                 llvm::Value *obj) {
  auto *callee = enum_method_callee(obj_sem, method);
  if (!callee)
    return nullptr;
  return emit_call(callee, obj,
                   emit_arguments(node, enum_method_signature(obj_sem, method),
                                  true));
}

// Int has no function: an enum's ordinal is the value itself.
llvm::Function *CodeGen::enum_method_callee(const TypePtr &enum_sem,
                                            const std::string &method) {
  if (method == "String")
    return enum_string_fn(enum_sem);
  if (method == "Int")
    return nullptr;
  auto &info = std::get<EnumTypeInfo>(enum_sem->detail);
  std::string origin =
      info.origin_package.empty() ? package_name : info.origin_package;
  std::string link = mangle(origin, info.name + "__" + method);
  if (auto *callee = module->getFunction(link))
    return callee;
  auto *fi = enum_method_signature(enum_sem, method);
  return fi ? declare_function(link, lower_signature(*fi, i64_type)) : nullptr;
}

const FuncTypeInfo *CodeGen::enum_method_signature(const TypePtr &enum_sem,
                                                   const std::string &method) {
  auto tm_it = analyzer.type_methods_.find(enum_sem.get());
  return tm_it == analyzer.type_methods_.end()
             ? nullptr
             : method_signature(tm_it->second, method);
}

llvm::Function *CodeGen::enum_string_fn(const TypePtr &enum_sem) {
  auto &info = std::get<EnumTypeInfo>(enum_sem->detail);
  std::string link =
      mangle(info.origin_package.empty() ? package_name : info.origin_package,
             info.name + "__EnumString");
  if (auto *fn = module->getFunction(link))
    return fn;

  auto *ptr_ty = llvm::PointerType::getUnqual(context);
  auto *ft = llvm::FunctionType::get(ptr_ty, {i64_type}, false);
  auto *fn =
      llvm::Function::Create(ft, llvm::Function::PrivateLinkage, link,
                             module.get());

  auto *saved_block = builder.GetInsertBlock();
  auto saved_point = builder.GetInsertPoint();

  auto *i64_ty = llvm::Type::getInt64Ty(context);
  auto *entry = llvm::BasicBlock::Create(context, "entry", fn);
  auto *dflt = llvm::BasicBlock::Create(context, "default", fn);
  builder.SetInsertPoint(entry);
  auto *sw = builder.CreateSwitch(fn->getArg(0), dflt, info.variants.size());
  for (auto &v : info.variants) {
    auto *bb = llvm::BasicBlock::Create(context, "v", fn);
    sw->addCase(llvm::ConstantInt::get(i64_ty, v.index), bb);
    builder.SetInsertPoint(bb);
    builder.CreateRet(make_string_constant(
        v.string_value.empty() ? v.name : v.string_value));
  }
  builder.SetInsertPoint(dflt);
  builder.CreateRet(make_string_constant(""));

  if (saved_block)
    builder.SetInsertPoint(saved_block, saved_point);
  return fn;
}

llvm::Value *CodeGen::emit_enum_from(const CallExprNode &node,
                                     const TypePtr &enum_sem) {
  auto &info = std::get<EnumTypeInfo>(enum_sem->detail);
  auto result_union =
      make_union_type({enum_sem, analyzer.builtins.error_base});
  auto *func = builder.GetInsertBlock()->getParent();
  auto *ptr_ty = llvm::PointerType::getUnqual(context);
  auto *i64_ty = llvm::Type::getInt64Ty(context);

  llvm::Value *arg = node.args.empty() ? nullptr : emit_expr(*node.args[0]);
  if (!arg)
    return nullptr;

  auto *miss_bb = llvm::BasicBlock::Create(context, "from.miss", func);
  auto *merge_bb = llvm::BasicBlock::Create(context, "from.merge", func);
  std::vector<std::pair<llvm::Value *, llvm::BasicBlock *>> incomings;

  if (info.string_backed) {
    auto *cmp_fn = module->getFunction("saga_string_compare");
    if (info.variants.empty())
      builder.CreateBr(miss_bb);
    for (size_t i = 0; i < info.variants.size(); ++i) {
      auto &v = info.variants[i];
      auto *hit_bb = llvm::BasicBlock::Create(context, "from.hit", func);
      auto *next_bb = (i + 1 < info.variants.size())
                          ? llvm::BasicBlock::Create(context, "from.test", func)
                          : miss_bb;
      auto *sv = make_string_constant(
          v.string_value.empty() ? v.name : v.string_value);
      auto *cmp = builder.CreateCall(cmp_fn, {arg, sv}, "from.cmp");
      auto *eq = builder.CreateICmpEQ(cmp, llvm::ConstantInt::get(i64_ty, 0),
                                      "from.eq");
      builder.CreateCondBr(eq, hit_bb, next_bb);

      builder.SetInsertPoint(hit_bb);
      auto *wrapped = emit_union_wrap(
          llvm::ConstantInt::get(i64_ty, v.index), enum_sem, result_union);
      incomings.push_back({wrapped, builder.GetInsertBlock()});
      builder.CreateBr(merge_bb);

      builder.SetInsertPoint(next_bb);
    }
  } else {
    auto *ok_bb = llvm::BasicBlock::Create(context, "from.ok", func);
    auto *sw = builder.CreateSwitch(arg, miss_bb, info.variants.size());
    for (auto &v : info.variants)
      sw->addCase(llvm::ConstantInt::get(i64_ty, v.index), ok_bb);
    builder.SetInsertPoint(ok_bb);
    auto *wrapped = emit_union_wrap(arg, enum_sem, result_union);
    incomings.push_back({wrapped, builder.GetInsertBlock()});
    builder.CreateBr(merge_bb);
  }

  builder.SetInsertPoint(miss_bb);
  llvm_type(analyzer.builtins.missing_type);
  auto &missing_info =
      std::get<StructTypeInfo>(analyzer.builtins.missing_type->detail);
  auto *err_box =
      emit_error_singleton(missing_info, "no matching enum variant");
  auto *miss_wrapped =
      emit_union_wrap(err_box, analyzer.builtins.error_base, result_union);
  incomings.push_back({miss_wrapped, builder.GetInsertBlock()});
  builder.CreateBr(merge_bb);

  builder.SetInsertPoint(merge_bb);
  auto *phi = builder.CreatePHI(ptr_ty, incomings.size(), "from.union");
  for (auto &[val, bb] : incomings)
    phi->addIncoming(val, bb);
  return phi;
}

} // namespace saga

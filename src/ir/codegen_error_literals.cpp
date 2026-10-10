// Copyright 2026 Rob Thornton
// SPDX-License-Identifier: MIT

// What an error literal needs beyond a struct's: the shared box it is built in,
// the constant one every literal with the same fixed message shares, and the
// message default, which may read the error's own fields.

#include "ir/codegen.hpp"

#include <llvm/IR/Constants.h>

namespace saga {

// The type_id in field 0 is what `is` and `==` tell errors apart by.
llvm::Value *CodeGen::emit_error_box(const TypePtr &sem, llvm::StructType *st) {
  auto &info = std::get<StructTypeInfo>(sem->detail);
  auto *box = emit_shared_box(sem, st);
  builder.CreateStore(
      llvm::ConstantInt::get(i64_type,
                             static_cast<uint64_t>(error_type_id(info))),
      builder.CreateStructGEP(st, box, 0, "type_id"));
  return box;
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
  auto *value = llvm::ConstantStruct::get(
      st, {llvm::ConstantInt::get(i64_type, tid), msg});
  auto *global = new llvm::GlobalVariable(
      *module, constant_shared_type(st), /*isConstant=*/true,
      llvm::GlobalValue::PrivateLinkage, constant_shared_box(value),
      info.name + ".singleton");
  global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
  auto *error = llvm::ConstantExpr::getInBoundsGetElementPtr(
      global->getValueType(), global,
      llvm::ArrayRef<llvm::Constant *>{builder.getInt32(0),
                                       builder.getInt32(2)});
  error_singletons[key] = error;
  return error;
}

// A shared box's header followed by its value: { refcount, ops, value }.
llvm::StructType *CodeGen::constant_shared_type(llvm::Type *value_ll) {
  auto *ptr = llvm::PointerType::getUnqual(context);
  return llvm::StructType::get(context, {i64_type, ptr, value_ll});
}

// A constant's count is -1, so nothing retains or frees it.
llvm::Constant *CodeGen::constant_shared_box(llvm::Constant *value) {
  return llvm::ConstantStruct::get(
      constant_shared_type(value->getType()),
      {llvm::ConstantInt::getSigned(i64_type, -1),
       llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(context)),
       value});
}

} // namespace saga

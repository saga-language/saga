// Copyright 2026 Rob Thornton
// SPDX-License-Identifier: MIT

// String literals: a plain one is a static constant, and an interpolated one
// is built by concatenating its pieces, each made a string first.

#include "ir/codegen.hpp"

#include <llvm/IR/Constants.h>

namespace saga {

llvm::Value *CodeGen::emit_string_literal(const StringLiteralNode &node) {
  if (interpolates(node))
    return emit_interpolation(node);
  std::string text;
  for (auto &frag : node.fragments)
    text += unescape_string_fragment(std::get<StringFragmentNode>(frag->data));
  return make_string_constant(text);
}

// The literal hands over a string of its own: a lone piece read out of a
// value takes a reference for it.
llvm::Value *CodeGen::emit_interpolation(const StringLiteralNode &node) {
  std::optional<InterpPiece> result;
  for (auto &frag : node.fragments) {
    auto piece = interpolation_piece(*frag);
    if (piece.val)
      result = result ? concat_pieces(*result, piece) : piece;
  }
  if (!result)
    return make_string_constant("");
  if (!result->made)
    emit_retain(result->val, analyzer.builtins.string_type);
  return result->val;
}

CodeGen::InterpPiece CodeGen::interpolation_piece(const Node &frag) {
  if (auto *sf = std::get_if<StringFragmentNode>(&frag.data)) {
    auto text = unescape_string_fragment(*sf);
    return {text.empty() ? nullptr : make_string_constant(text), false};
  }
  auto sem = semantic_type(frag);
  auto *val = emit_borrowed(frag);
  if (sem && sem->kind == TypeKind::String)
    return {val, false};
  return {emit_to_string(val, sem), true};
}

// The concatenation copies both sides, so a piece made for it is done with.
CodeGen::InterpPiece CodeGen::concat_pieces(const InterpPiece &left,
                                            const InterpPiece &right) {
  auto *joined = builder.CreateCall(module->getFunction("saga_string_concat"),
                                    {left.val, right.val}, "interp");
  for (auto *piece : {&left, &right})
    if (piece->made)
      emit_release(piece->val, analyzer.builtins.string_type);
  return {joined, true};
}

llvm::Value *CodeGen::emit_to_string(llvm::Value *val, const TypePtr &sem) {
  if (!val || !sem)
    return val;

  switch (sem->kind) {
  case TypeKind::String:
    return val; // Already a string pointer.
  case TypeKind::Int: {
    auto *fn = module->getFunction("saga_int_to_string");
    return builder.CreateCall(fn, {val}, "istr");
  }
  case TypeKind::Float: {
    auto *fn = module->getFunction("saga_float_to_string");
    return builder.CreateCall(fn, {val}, "fstr");
  }
  case TypeKind::Bool: {
    auto *ext = builder.CreateZExt(val, i64_type, "bext");
    auto *fn = module->getFunction("saga_bool_to_string");
    return builder.CreateCall(fn, {ext}, "bstr");
  }
  default:
    // For types we can't convert, return an empty string placeholder.
    return make_string_constant("");
  }
}

llvm::Value *CodeGen::make_string_constant(const std::string &text) {
  auto it = string_constants.find(text);
  if (it != string_constants.end())
    return it->second;

  auto *char_array =
      llvm::ConstantDataArray::getString(context, text, /*AddNull=*/false);
  auto *raw_global = new llvm::GlobalVariable(
      *module, char_array->getType(), true,
      llvm::GlobalValue::PrivateLinkage, char_array, ".str");
  raw_global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
  raw_global->setAlignment(llvm::Align(1));

  auto *data_ptr = llvm::ConstantExpr::getInBoundsGetElementPtr(
      char_array->getType(), raw_global,
      llvm::ArrayRef<llvm::Constant *>{
          llvm::ConstantInt::get(i64_type, 0),
          llvm::ConstantInt::get(i64_type, 0)});
  auto *length = llvm::ConstantInt::get(i64_type, text.size());
  auto *refcount = llvm::ConstantInt::getSigned(i64_type, -1); // static
  auto *str_const =
      llvm::ConstantStruct::get(string_type, {data_ptr, length, refcount});

  auto *str_global = new llvm::GlobalVariable(
      *module, string_type, true,
      llvm::GlobalValue::PrivateLinkage, str_const, ".saga_runtime_str");
  str_global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);

  string_constants[text] = str_global;
  return str_global;
}

} // namespace saga

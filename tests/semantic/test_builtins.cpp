// Copywrite 2026 Rob Thornton
// SPDX-License-Identifier: MIT

#include "semantic/builtins.hpp"

#include <gtest/gtest.h>

namespace saga {

TEST(Builtins, TypesInit) {
  BuiltinTypes types;
  types.init();
  EXPECT_NE(types.void_type, nullptr);
  EXPECT_NE(types.bool_type, nullptr);
  EXPECT_NE(types.int_type, nullptr);
  EXPECT_NE(types.float_type, nullptr);
  EXPECT_NE(types.string_type, nullptr);
  EXPECT_NE(types.invalid_type, nullptr);
}

TEST(Builtins, SizedIntegerTypes) {
  BuiltinTypes types;
  types.init();
  EXPECT_NE(types.int8_type, nullptr);
  EXPECT_NE(types.int16_type, nullptr);
  EXPECT_NE(types.int32_type, nullptr);
  EXPECT_NE(types.int64_type, nullptr);
  EXPECT_NE(types.uint8_type, nullptr);
  EXPECT_NE(types.uint16_type, nullptr);
  EXPECT_NE(types.uint32_type, nullptr);
  EXPECT_NE(types.uint64_type, nullptr);

  // Byte is Uint8
  EXPECT_TRUE(types_equal(types.byte_type, types.uint8_type));
}

TEST(Builtins, SizedFloatTypes) {
  BuiltinTypes types;
  types.init();
  EXPECT_NE(types.float32_type, nullptr);
  EXPECT_NE(types.float64_type, nullptr);
}

TEST(Builtins, BaseError) {
  BuiltinTypes types;
  types.init();
  EXPECT_NE(types.error_base, nullptr);
  EXPECT_EQ(types.error_base->kind, TypeKind::Struct);

  auto &error_info = std::get<StructTypeInfo>(types.error_base->detail);
  EXPECT_EQ(error_info.name, "error");
  EXPECT_TRUE(error_info.is_error);
  EXPECT_TRUE(error_info.methods.empty());
  // Every error box carries the { type_id, message } prefix.
  ASSERT_EQ(error_info.fields.size(), 2u);
  EXPECT_EQ(error_info.fields[0].name, "type_id");
  EXPECT_EQ(error_info.fields[1].name, "message");

  EXPECT_NE(types.iterable_iface, nullptr);
  EXPECT_EQ(types.iterable_iface->kind, TypeKind::Interface);

  auto &iter_info =
      std::get<InterfaceTypeInfo>(types.iterable_iface->detail);
  EXPECT_EQ(iter_info.name, "Iterable");
  EXPECT_EQ(iter_info.type_params.size(), 1u);
  EXPECT_EQ(iter_info.type_params[0].name, "T");
}

TEST(Builtins, MissingType) {
  BuiltinTypes types;
  types.init();
  EXPECT_NE(types.missing_type, nullptr);
  EXPECT_EQ(types.missing_type->kind, TypeKind::Struct);

  auto &info = std::get<StructTypeInfo>(types.missing_type->detail);
  EXPECT_EQ(info.name, "Missing");
  // Missing is a built-in error, not a method-bearing struct.
  EXPECT_TRUE(info.is_error);
  EXPECT_TRUE(info.methods.empty());
}

TEST(Builtins, ComparisonEnum) {
  BuiltinTypes types;
  types.init();
  EXPECT_NE(types.comparison_type, nullptr);
  EXPECT_EQ(types.comparison_type->kind, TypeKind::Enum);

  auto &info = std::get<EnumTypeInfo>(types.comparison_type->detail);
  EXPECT_EQ(info.name, "Comparison");
  ASSERT_EQ(info.variants.size(), 3u);
  EXPECT_EQ(info.variants[0].name, "Less");
  EXPECT_EQ(info.variants[1].name, "Equal");
  EXPECT_EQ(info.variants[2].name, "Greater");
}

TEST(Builtins, TaskAndContextTypes) {
  BuiltinTypes types;
  types.init();
  EXPECT_NE(types.task_type, nullptr);
  EXPECT_EQ(types.task_type->kind, TypeKind::Struct);

  auto &task_info = std::get<StructTypeInfo>(types.task_type->detail);
  EXPECT_EQ(task_info.name, "Task");
  EXPECT_GE(task_info.methods.size(), 4u); // Alive?, Cancel, Term, Wait

  EXPECT_NE(types.context_type, nullptr);
  EXPECT_EQ(types.context_type->kind, TypeKind::Struct);

  auto &ctx_info = std::get<StructTypeInfo>(types.context_type->detail);
  EXPECT_EQ(ctx_info.name, "Context");
  EXPECT_GE(ctx_info.methods.size(), 3u); // Cancelled?, Exit, Send
}

TEST(Builtins, RegisterPopulatesScope) {
  auto scope = std::make_shared<Scope>(ScopeKind::Global);
  BuiltinTypes types;
  register_builtins(scope, types);

  // Primitive type names.
  EXPECT_TRUE(scope->lookup("int").has_value());
  EXPECT_TRUE(scope->lookup("float").has_value());
  EXPECT_TRUE(scope->lookup("bool").has_value());
  EXPECT_TRUE(scope->lookup("string").has_value());
  EXPECT_TRUE(scope->lookup("void").has_value());
  EXPECT_TRUE(scope->lookup("byte").has_value());

  // Sized integers.
  EXPECT_TRUE(scope->lookup("int8").has_value());
  EXPECT_TRUE(scope->lookup("int16").has_value());
  EXPECT_TRUE(scope->lookup("int32").has_value());
  EXPECT_TRUE(scope->lookup("int64").has_value());
  EXPECT_TRUE(scope->lookup("uint8").has_value());
  EXPECT_TRUE(scope->lookup("uint16").has_value());
  EXPECT_TRUE(scope->lookup("uint32").has_value());
  EXPECT_TRUE(scope->lookup("uint64").has_value());

  // Sized floats.
  EXPECT_TRUE(scope->lookup("float32").has_value());
  EXPECT_TRUE(scope->lookup("float64").has_value());

  // Internal interfaces.
  EXPECT_TRUE(scope->lookup("error").has_value());
  EXPECT_TRUE(scope->lookup("Iterable").has_value());

  // Internal structs.
  EXPECT_TRUE(scope->lookup("Missing").has_value());
  EXPECT_TRUE(scope->lookup("Task").has_value());
  EXPECT_TRUE(scope->lookup("Context").has_value());

  // Internal enum.
  EXPECT_TRUE(scope->lookup("Comparison").has_value());

  // Bool constants.
  EXPECT_TRUE(scope->lookup("true").has_value());
  EXPECT_TRUE(scope->lookup("false").has_value());

  // No free functions in the global scope.
  EXPECT_FALSE(scope->lookup("print").has_value());
  EXPECT_FALSE(scope->lookup("len").has_value());
}

TEST(Builtins, TypeSymbolKinds) {
  auto scope = std::make_shared<Scope>(ScopeKind::Global);
  BuiltinTypes types;
  register_builtins(scope, types);

  auto int_sym = scope->lookup("int");
  ASSERT_TRUE(int_sym.has_value());
  EXPECT_EQ(int_sym->kind, SymbolKind::Type);
  EXPECT_TRUE(int_sym->is_builtin);

  auto true_sym = scope->lookup("true");
  ASSERT_TRUE(true_sym.has_value());
  EXPECT_EQ(true_sym->kind, SymbolKind::Constant);
  EXPECT_TRUE(true_sym->is_builtin);

  auto error_sym = scope->lookup("error");
  ASSERT_TRUE(error_sym.has_value());
  EXPECT_EQ(error_sym->kind, SymbolKind::Type);
  EXPECT_EQ(error_sym->type->kind, TypeKind::Struct);
  EXPECT_TRUE(std::get<StructTypeInfo>(error_sym->type->detail).is_error);
}

// String, Int, Float, Bool methods are fully migrated to stdlib packages.
// builtin_methods() returns empty for these types.
TEST(Builtins, StringMethodsMigratedToStdlib) {
  BuiltinTypes types;
  types.init();
  auto methods = builtin_methods(TypeKind::String, types);
  EXPECT_TRUE(methods.empty());
}

TEST(Builtins, IntMethodsMigratedToStdlib) {
  BuiltinTypes types;
  types.init();
  auto methods = builtin_methods(TypeKind::Int, types);
  EXPECT_TRUE(methods.empty());
}

TEST(Builtins, FloatMethodsMigratedToStdlib) {
  BuiltinTypes types;
  types.init();
  auto methods = builtin_methods(TypeKind::Float, types);
  EXPECT_TRUE(methods.empty());
}

TEST(Builtins, BoolMethodsMigratedToStdlib) {
  BuiltinTypes types;
  types.init();
  auto methods = builtin_methods(TypeKind::Bool, types);
  EXPECT_TRUE(methods.empty());
}

// Array and Map methods are fully migrated to stdlib (std/array, std/map).
TEST(Builtins, ArrayMethodsMigratedToStdlib) {
  BuiltinTypes types;
  types.init();
  auto methods = builtin_methods(TypeKind::Array, types);
  EXPECT_TRUE(methods.empty());
}

TEST(Builtins, MapMethodsMigratedToStdlib) {
  BuiltinTypes types;
  types.init();
  auto methods = builtin_methods(TypeKind::Map, types);
  EXPECT_TRUE(methods.empty());
}

} // namespace saga

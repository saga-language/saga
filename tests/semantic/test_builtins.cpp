// Copywrite 2026 Rob Thornton
// SPDX-License-Identifier: MIT

#include "semantic/builtins.hpp"

#include <gtest/gtest.h>

namespace mc {

TEST(Builtins, TypesInit) {
  BuiltinTypes types;
  types.init();
  EXPECT_NE(types.void_type, nullptr);
  EXPECT_NE(types.bool_type, nullptr);
  EXPECT_NE(types.int_type, nullptr);
  EXPECT_NE(types.float_type, nullptr);
  EXPECT_NE(types.string_type, nullptr);
  EXPECT_NE(types.error_type, nullptr);
}

TEST(Builtins, RegisterPopulatesScope) {
  auto scope = std::make_shared<Scope>(ScopeKind::Global);
  BuiltinTypes types;
  register_builtins(scope, types);

  // Primitive type names are registered.
  EXPECT_TRUE(scope->lookup("Int").has_value());
  EXPECT_TRUE(scope->lookup("Float").has_value());
  EXPECT_TRUE(scope->lookup("Bool").has_value());
  EXPECT_TRUE(scope->lookup("String").has_value());
  EXPECT_TRUE(scope->lookup("Void").has_value());

  // Bool constants.
  EXPECT_TRUE(scope->lookup("true").has_value());
  EXPECT_TRUE(scope->lookup("false").has_value());

  // No free functions in the global scope — those come from the stdlib.
  EXPECT_FALSE(scope->lookup("print").has_value());
  EXPECT_FALSE(scope->lookup("len").has_value());
}

TEST(Builtins, TypeSymbolKinds) {
  auto scope = std::make_shared<Scope>(ScopeKind::Global);
  BuiltinTypes types;
  register_builtins(scope, types);

  auto int_sym = scope->lookup("Int");
  ASSERT_TRUE(int_sym.has_value());
  EXPECT_EQ(int_sym->kind, SymbolKind::Type);
  EXPECT_TRUE(int_sym->is_builtin);

  auto true_sym = scope->lookup("true");
  ASSERT_TRUE(true_sym.has_value());
  EXPECT_EQ(true_sym->kind, SymbolKind::Constant);
  EXPECT_TRUE(true_sym->is_builtin);
}

TEST(Builtins, StringMethods) {
  BuiltinTypes types;
  types.init();
  auto methods = builtin_methods(TypeKind::String, types);
  EXPECT_FALSE(methods.empty());

  // Check that "len" is present.
  bool has_len = false;
  for (auto &m : methods) {
    if (m.name == "len")
      has_len = true;
  }
  EXPECT_TRUE(has_len);
}

TEST(Builtins, ArrayMethods) {
  BuiltinTypes types;
  types.init();
  auto methods = builtin_methods(TypeKind::Array, types);
  bool has_len = false;
  for (auto &m : methods) {
    if (m.name == "len")
      has_len = true;
  }
  EXPECT_TRUE(has_len);
}



} // namespace mc

// Copywrite 2026 Rob Thornton
// SPDX-License-Identifier: MIT

#include "semantic/scope.hpp"

#include <gtest/gtest.h>

namespace mc {

TEST(Scope, DeclareAndLookup) {
  auto scope = std::make_shared<Scope>(ScopeKind::Global);
  auto sym = Symbol::variable("x", make_int_type(), Span{0, 1});
  EXPECT_TRUE(scope->declare(sym));

  auto found = scope->lookup("x");
  ASSERT_TRUE(found.has_value());
  EXPECT_EQ(found->name, "x");
  EXPECT_EQ(found->kind, SymbolKind::Variable);
}

TEST(Scope, DuplicateDeclaration) {
  auto scope = std::make_shared<Scope>(ScopeKind::Global);
  auto sym = Symbol::variable("x", make_int_type(), Span{0, 1});
  EXPECT_TRUE(scope->declare(sym));
  EXPECT_FALSE(scope->declare(sym)); // duplicate
}

TEST(Scope, LookupMissing) {
  auto scope = std::make_shared<Scope>(ScopeKind::Global);
  EXPECT_FALSE(scope->lookup("nonexistent").has_value());
}

TEST(Scope, NestedLookup) {
  auto parent = std::make_shared<Scope>(ScopeKind::Global);
  parent->declare(Symbol::variable("x", make_int_type(), Span{0, 1}));

  auto child = parent->child(ScopeKind::Block);
  child->declare(Symbol::variable("y", make_string_type(), Span{2, 3}));

  // Child can see parent's symbol.
  auto found_x = child->lookup("x");
  ASSERT_TRUE(found_x.has_value());
  EXPECT_EQ(found_x->name, "x");

  // Child can see its own symbol.
  auto found_y = child->lookup("y");
  ASSERT_TRUE(found_y.has_value());
  EXPECT_EQ(found_y->name, "y");

  // Parent cannot see child's symbol.
  EXPECT_FALSE(parent->lookup("y").has_value());
}

TEST(Scope, Shadowing) {
  auto parent = std::make_shared<Scope>(ScopeKind::Global);
  parent->declare(Symbol::variable("x", make_int_type(), Span{0, 1}));

  auto child = parent->child(ScopeKind::Block);
  child->declare(Symbol::variable("x", make_string_type(), Span{2, 3}));

  // Child sees the shadowed (String) version.
  auto found = child->lookup("x");
  ASSERT_TRUE(found.has_value());
  EXPECT_EQ(found->type->kind, TypeKind::String);

  // Parent still sees the original (Int) version.
  auto found_parent = parent->lookup("x");
  ASSERT_TRUE(found_parent.has_value());
  EXPECT_EQ(found_parent->type->kind, TypeKind::Int);
}

TEST(Scope, LookupLocal) {
  auto parent = std::make_shared<Scope>(ScopeKind::Global);
  parent->declare(Symbol::variable("x", make_int_type(), Span{0, 1}));

  auto child = parent->child(ScopeKind::Block);

  // lookup_local does NOT walk parents.
  EXPECT_FALSE(child->lookup_local("x").has_value());

  child->declare(Symbol::variable("y", make_string_type(), Span{2, 3}));
  EXPECT_TRUE(child->lookup_local("y").has_value());
}

TEST(Scope, DeeplyNested) {
  auto global = std::make_shared<Scope>(ScopeKind::Global);
  global->declare(Symbol::variable("g", make_int_type(), Span{0, 0}));

  auto func = global->child(ScopeKind::Function);
  func->declare(Symbol::variable("f", make_string_type(), Span{0, 0}));

  auto loop = func->child(ScopeKind::Loop);
  auto block = loop->child(ScopeKind::Block);

  // Deepest scope can see all ancestors.
  EXPECT_TRUE(block->lookup("g").has_value());
  EXPECT_TRUE(block->lookup("f").has_value());
}

TEST(Scope, NearestKind) {
  auto global = std::make_shared<Scope>(ScopeKind::Global);
  auto func = global->child(ScopeKind::Function);
  auto loop = func->child(ScopeKind::Loop);
  auto block = loop->child(ScopeKind::Block);

  EXPECT_TRUE(block->is_inside(ScopeKind::Loop));
  EXPECT_TRUE(block->is_inside(ScopeKind::Function));
  EXPECT_TRUE(block->is_inside(ScopeKind::Global));
  EXPECT_FALSE(block->is_inside(ScopeKind::Spawn));

  auto nearest_func = block->nearest(ScopeKind::Function);
  ASSERT_NE(nearest_func, nullptr);
  EXPECT_EQ(nearest_func->kind, ScopeKind::Function);
}

TEST(Scope, TypeBindings) {
  auto global = std::make_shared<Scope>(ScopeKind::Global);
  auto func = global->child(ScopeKind::Function);
  func->type_bindings[0] = make_int_type();

  auto block = func->child(ScopeKind::Block);
  block->type_bindings[1] = make_string_type();

  auto all = block->all_type_bindings();
  EXPECT_EQ(all.size(), 2u);
  EXPECT_EQ(all[0]->kind, TypeKind::Int);
  EXPECT_EQ(all[1]->kind, TypeKind::String);
}

TEST(Scope, TypeBindingsInnerOverrides) {
  auto parent = std::make_shared<Scope>(ScopeKind::Global);
  parent->type_bindings[0] = make_int_type();

  auto child = parent->child(ScopeKind::Block);
  child->type_bindings[0] = make_string_type(); // override

  auto all = child->all_type_bindings();
  EXPECT_EQ(all.size(), 1u);
  EXPECT_EQ(all[0]->kind, TypeKind::String);
}

} // namespace mc

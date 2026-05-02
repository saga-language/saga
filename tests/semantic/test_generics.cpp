// Copywrite 2026 Rob Thornton
// SPDX-License-Identifier: MIT

#include "semantic/analyzer.hpp"
#include "frontend/file.hpp"
#include "frontend/fileset.hpp"
#include "frontend/parser.hpp"

#include <gtest/gtest.h>

namespace saga {

// ---------------------------------------------------------------------------
// Helper
// ---------------------------------------------------------------------------

struct GR {
  FileSet fileset;
  NodePtr ast;
  std::unique_ptr<Analyzer> analyzer;

  static GR from(const std::string &source) {
    GR r;
    r.fileset.add_file(File::from_source("test.sg", source));
    Parser parser(r.fileset);
    r.ast = parser.parse();
    r.analyzer = std::make_unique<Analyzer>(r.fileset);
    if (r.ast)
      r.analyzer->analyze(*r.ast);
    return r;
  }

  bool ok() const { return analyzer->errors.errors.empty(); }

  bool has_err(const std::string &substr) const {
    return analyzer->has_error_containing(substr);
  }
};

// ===========================================================================
// Generic function declarations
// ===========================================================================

TEST(Generics, SingleTypeParamFunc) {
  auto r = GR::from(
      "fn |T| Identity(x T) T { x }\n"
      "fn f() Int { Identity(42) }");
  EXPECT_TRUE(r.ok());
}

TEST(Generics, TwoTypeParamFunc) {
  auto r = GR::from(
      "fn |T, U| Pair(a T, b U) T { a }\n"
      "fn f() Int { Pair(1, \"hello\") }");
  EXPECT_TRUE(r.ok());
}

TEST(Generics, GenericFuncReturnTypeInferred) {
  // Identity(42) should return Int, which is assigned to an Int variable.
  auto r = GR::from(
      "fn |T| Identity(x T) T { x }\n"
      "fn f() Int {\n"
      "  result := Identity(42)\n"
      "  result\n"
      "}");
  EXPECT_TRUE(r.ok());
}

TEST(Generics, GenericFuncTypeMismatchInBody) {
  // The generic body uses T correctly.
  auto r = GR::from("fn |T| Identity(x T) T { x }");
  EXPECT_TRUE(r.ok());
}

TEST(Generics, GenericFuncDefinitionOnly) {
  // Just declaring a generic function should not error.
  auto r = GR::from("fn |T| Wrap(x T) [T] { [x] }");
  EXPECT_TRUE(r.ok());
}

// ===========================================================================
// Generic struct declarations
// ===========================================================================

TEST(Generics, GenericStructDecl) {
  auto r = GR::from("struct |T| Box { value T }");
  EXPECT_TRUE(r.ok());
}

TEST(Generics, GenericStructInstantiationInferred) {
  auto r = GR::from(
      "struct |T| Box { value T }\n"
      "fn f() { Box{value: 42} }");
  EXPECT_TRUE(r.ok());
}

TEST(Generics, GenericStructFieldAccess) {
  // After instantiation, the field type should be concrete.
  auto r = GR::from(
      "struct |T| Box { value T }\n"
      "fn f() Int {\n"
      "  b := Box{value: 42}\n"
      "  b.value\n"
      "}");
  EXPECT_TRUE(r.ok());
}

TEST(Generics, GenericStructStringInferred) {
  auto r = GR::from(
      "struct |T| Box { value T }\n"
      "fn f() String {\n"
      "  b := Box{value: \"hello\"}\n"
      "  b.value\n"
      "}");
  EXPECT_TRUE(r.ok());
}

TEST(Generics, GenericStructMultipleTypeParams) {
  auto r = GR::from(
      "struct |K, V| Pair {\n"
      "  key K\n"
      "  value V\n"
      "}\n"
      "fn f() {\n"
      "  p := Pair{key: \"name\", value: 42}\n"
      "}");
  EXPECT_TRUE(r.ok());
}

// ===========================================================================
// Generic function expressions (closures)
// ===========================================================================

TEST(Generics, GenericFuncExpr) {
  auto r = GR::from(
      "fn f() {\n"
      "  id := fn |T| (x T) T { x }\n"
      "}");
  EXPECT_TRUE(r.ok());
}

// ===========================================================================
// Generic interface declarations
// ===========================================================================

TEST(Generics, GenericInterfaceDecl) {
  auto r = GR::from(
      "interface |T| Container {\n"
      "  Get() T\n"
      "  Set(val T) Void\n"
      "}");
  EXPECT_TRUE(r.ok());
}

// ===========================================================================
// Type parameter usage in function bodies
// ===========================================================================

TEST(Generics, TypeParamInArrayReturn) {
  auto r = GR::from("fn |T| Wrap(x T) [T] { [x] }");
  EXPECT_TRUE(r.ok());
}

TEST(Generics, TypeParamInMapReturn) {
  // Ensure map types with type params resolve.
  auto r = GR::from(
      "fn |K, V| MakePair(k K, v V) {K: V} {\n"
      "  {k: v}\n"
      "}");
  EXPECT_TRUE(r.ok());
}

// ===========================================================================
// Inference failures
// ===========================================================================

TEST(Generics, InferenceConflict) {
  // T is used for two params that get different types.
  auto r = GR::from(
      "fn |T| Same(a T, b T) T { a }\n"
      "fn f() { Same(1, \"two\") }");
  EXPECT_TRUE(r.has_err("cannot infer"));
}

// ===========================================================================
// has_type_params utility
// ===========================================================================

TEST(Generics, HasTypeParamsTrue) {
  auto tp = make_type_param(0, "T");
  auto fn = make_func_type({tp}, {tp});
  EXPECT_TRUE(has_type_params(fn));
}

TEST(Generics, HasTypeParamsFalse) {
  auto fn = make_func_type({make_int_type()}, {make_int_type()});
  EXPECT_FALSE(has_type_params(fn));
}

TEST(Generics, HasTypeParamsNested) {
  auto tp = make_type_param(0, "T");
  auto arr = make_array_type(tp);
  EXPECT_TRUE(has_type_params(arr));
}

TEST(Generics, HasTypeParamsNull) {
  EXPECT_FALSE(has_type_params(nullptr));
}

// ===========================================================================
// Generic spawn (channel type annotation)
// ===========================================================================

TEST(Generics, SpawnWithGeneric) {
  auto r = GR::from(
      "fn f() {\n"
      "  |String| spawn |task| { task }\n"
      "}");
  EXPECT_TRUE(r.ok());
}

// ===========================================================================
// P5: type-parameter shadowing (method on generic struct must not reuse the
// struct's type-param names in its own |...| clause)
// ===========================================================================

TEST(Generics, MethodTypeParamShadowsStructParam) {
  auto r = GR::from(
      "struct |T| Box {\n"
      "  value T\n"
      "  fn |T| Map(x T) Void { }\n"
      "}");
  EXPECT_TRUE(r.has_err("shadows enclosing struct type parameter"));
}

TEST(Generics, StandaloneReceiverMethodReusingStructParamIsOk) {
  // Standalone receiver methods may reuse the struct's type-param name in
  // their own |...| clause — that's the binding point for the receiver's T.
  auto r = GR::from(
      "struct |T| Box { value T }\n"
      "pub fn |T| (b |T| Box) Get() T { b.value }");
  EXPECT_TRUE(r.ok());
}

// P5 invariant: instantiating a generic struct must preserve the
// origin_package on every method. Without this, codegen mangles
// imported method calls against the caller's package, which would
// produce undefined symbols at link time across packages.
TEST(Generics, InstantiatingGenericStructPreservesMethodOrigin) {
  auto r = GR::from(
      "struct |T| Box { value T }\n"
      "pub fn |T| (b |T| Box) Get() T { b.value }\n"
      "pub fn Main() Void { b := Box{value: 1}; v := b.Get() }");
  ASSERT_TRUE(r.ok()) << r.analyzer->errors.errors.size() << " errors";

  // Find the Box struct type in the package scope and confirm every
  // method has origin_package set. We don't pin a specific value here
  // (single-package tests use an empty current_package_name) — the
  // contract is "non-empty if the type was declared in this analyzer".
  auto sym = r.analyzer->package_scope_->symbols.find("Box");
  ASSERT_NE(sym, r.analyzer->package_scope_->symbols.end());
  ASSERT_TRUE(sym->second.type);
  auto &si = std::get<StructTypeInfo>(sym->second.type->detail);
  ASSERT_FALSE(si.methods.empty());
  std::string expected_origin = si.origin_package;
  for (auto &m : si.methods) {
    EXPECT_EQ(m.origin_package, expected_origin)
        << "method '" << m.name
        << "' lost its origin_package — instantiate_generic_struct or a "
           "method-construction site is dropping the field";
  }
}

} // namespace saga

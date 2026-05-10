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

struct TC {
  FileSet fileset;
  NodePtr ast;
  std::unique_ptr<Analyzer> analyzer;

  static TC from(const std::string &source, bool stdlib = true) {
    TC r;
    r.fileset.add_file(File::from_source("test.sg", source));
    Parser parser(r.fileset);
    r.ast = parser.parse();
    auto resolver = std::make_shared<PackageResolver>();
    resolver->sgi_search_paths.push_back(SAGA_STD_SGI_DIR);
    r.analyzer = std::make_unique<Analyzer>(r.fileset, resolver);
    r.analyzer->is_stdlib = stdlib;
    if (r.ast)
      r.analyzer->analyze(*r.ast);
    return r;
  }

  size_t error_count() const { return analyzer->errors.errors.size(); }
  bool ok() const { return error_count() == 0; }

  bool has_err(const std::string &substr) const {
    return analyzer->has_error_containing(substr);
  }
};

// ===========================================================================
// Literal type inference
// ===========================================================================

TEST(TypeCheck, IntLiteral) {
  auto r = TC::from("fn f() Int { 42 }");
  EXPECT_TRUE(r.ok());
}

TEST(TypeCheck, FloatLiteral) {
  auto r = TC::from("fn f() Float { 3.14 }");
  EXPECT_TRUE(r.ok());
}

TEST(TypeCheck, BoolLiteral) {
  auto r = TC::from("fn f() Bool { true }");
  EXPECT_TRUE(r.ok());
}

TEST(TypeCheck, StringLiteral) {
  auto r = TC::from("fn f() String { \"hello\" }");
  EXPECT_TRUE(r.ok());
}

// ===========================================================================
// Return type mismatch
// ===========================================================================

TEST(TypeCheck, ReturnTypeMismatch) {
  auto r = TC::from("fn f() Int { \"oops\" }");
  EXPECT_TRUE(r.has_err("return type"));
}

TEST(TypeCheck, ReturnStatementTypeMismatch) {
  auto r = TC::from("fn f() Int { return \"oops\" }");
  EXPECT_TRUE(r.has_err("return value"));
}

TEST(TypeCheck, ReturnStatementCorrect) {
  auto r = TC::from("fn f() Int { return 42 }");
  EXPECT_TRUE(r.ok());
}

TEST(TypeCheck, VoidFunctionNoReturn) {
  auto r = TC::from("fn f() { 42 }");
  EXPECT_TRUE(r.ok());
}

TEST(TypeCheck, ReturnMissingValue) {
  auto r = TC::from("fn f() Int { return }");
  EXPECT_TRUE(r.has_err("missing return value"));
}

TEST(TypeCheck, ReturnCountMismatch) {
  auto r = TC::from("fn f() Int, String { return 42 }");
  EXPECT_TRUE(r.has_err("return has 1 value(s), expected 2"));
}

// ===========================================================================
// Arithmetic operators
// ===========================================================================

TEST(TypeCheck, AddInts) {
  auto r = TC::from("fn f() Int { 1 + 2 }");
  EXPECT_TRUE(r.ok());
}

TEST(TypeCheck, AddIntFloat) {
  auto r = TC::from("fn f() Float { 1 + 2.0 }");
  EXPECT_TRUE(r.ok());
}

TEST(TypeCheck, AddStrings) {
  auto r = TC::from("fn f() String { \"a\" + \"b\" }");
  EXPECT_TRUE(r.ok());
}

TEST(TypeCheck, AddStringInt) {
  auto r = TC::from("fn f() { \"a\" + 1 }");
  EXPECT_TRUE(r.has_err("arithmetic operator requires numeric type"));
}

TEST(TypeCheck, SubBools) {
  auto r = TC::from("fn f() { true - false }");
  EXPECT_TRUE(r.has_err("arithmetic operator requires numeric type"));
}

// ===========================================================================
// Division returns impure type
// ===========================================================================

TEST(TypeCheck, DivisionReturnsUnion) {
  // Division returns T | Error.
  auto r = TC::from("fn f() { 10 / 2 }");
  EXPECT_TRUE(r.ok());
}

// ===========================================================================
// Comparison operators
// ===========================================================================

TEST(TypeCheck, EqualInts) {
  auto r = TC::from("fn f() Bool { 1 == 2 }");
  EXPECT_TRUE(r.ok());
}

TEST(TypeCheck, LessThanInts) {
  auto r = TC::from("fn f() Bool { 1 < 2 }");
  EXPECT_TRUE(r.ok());
}

TEST(TypeCheck, CompareStrings) {
  auto r = TC::from("fn f() Bool { \"a\" < \"b\" }");
  EXPECT_TRUE(r.ok());
}

// ===========================================================================
// Logical operators
// ===========================================================================

TEST(TypeCheck, LogicalAnd) {
  auto r = TC::from("fn f() Bool { true && false }");
  EXPECT_TRUE(r.ok());
}

TEST(TypeCheck, LogicalAndNonBool) {
  auto r = TC::from("fn f() { 1 && 2 }");
  EXPECT_TRUE(r.has_err("logical operator"));
}

// ===========================================================================
// Unary operators
// ===========================================================================

TEST(TypeCheck, UnaryNot) {
  auto r = TC::from("fn f() Bool { !true }");
  EXPECT_TRUE(r.ok());
}

TEST(TypeCheck, UnaryNotNonBool) {
  auto r = TC::from("fn f() { !42 }");
  EXPECT_TRUE(r.has_err("logical not"));
}

TEST(TypeCheck, UnaryNegate) {
  auto r = TC::from("fn f() Int { -42 }");
  EXPECT_TRUE(r.ok());
}

TEST(TypeCheck, UnaryNegateNonNumeric) {
  auto r = TC::from("fn f() { -\"oops\" }");
  EXPECT_TRUE(r.has_err("negation requires numeric type"));
}

// ===========================================================================
// If expression
// ===========================================================================

TEST(TypeCheck, IfConditionMustBeBool) {
  auto r = TC::from("fn f() { if 42 {} }");
  EXPECT_TRUE(r.has_err("condition"));
}

TEST(TypeCheck, IfExpressionType) {
  auto r = TC::from("fn f() Int { if true { 1 } else { 2 } }");
  EXPECT_TRUE(r.ok());
}

// ===========================================================================
// Variable declarations
// ===========================================================================

TEST(TypeCheck, VarDeclWithInit) {
  auto r = TC::from("fn f() Int {\n  x Int = 42\n  x\n}");
  EXPECT_TRUE(r.ok());
}

TEST(TypeCheck, VarDeclTypeMismatch) {
  auto r = TC::from("fn f() {\n  x Int = \"oops\"\n}");
  EXPECT_TRUE(r.has_err("variable initializer"));
}

TEST(TypeCheck, DeclAssignInfersType) {
  auto r = TC::from("fn f() Int {\n  x := 42\n  x\n}");
  EXPECT_TRUE(r.ok());
}

// Spec: `arr1 := []` is invalid — no declared type means no inferrable
// element type.  (docs/language.md:601)
TEST(TypeCheck, DeclAssign_EmptyArrayLiteral_Rejected) {
  auto r = TC::from("fn f() {\n  arr := []\n}");
  EXPECT_TRUE(r.has_err("empty array literal"));
}

TEST(TypeCheck, DeclAssign_EmptyMapLiteral_Rejected) {
  auto r = TC::from("fn f() {\n  m := {}\n}");
  EXPECT_TRUE(r.has_err("empty map literal"));
}

TEST(TypeCheck, VarDecl_EmptyArrayLiteral_Allowed) {
  // Typed declaration (`arr Int[] = []`) provides the element type, so
  // the empty literal is fine here even though `:=` would reject it.
  auto r = TC::from("fn f() {\n  arr Int[] = []\n}");
  EXPECT_TRUE(r.ok());
}

// ===========================================================================
// Assignment
// ===========================================================================

TEST(TypeCheck, AssignmentTypeMismatch) {
  auto r = TC::from("fn f() {\n  x Int = 0\n  x = \"oops\"\n}");
  EXPECT_TRUE(r.has_err("assignment"));
}

TEST(TypeCheck, CompoundAssignment) {
  auto r = TC::from("fn f() {\n  x := 10\n  x += 5\n}");
  EXPECT_TRUE(r.ok());
}

TEST(TypeCheck, StringConcatAssignment) {
  auto r = TC::from("fn f() {\n  s := \"hello\"\n  s += \" world\"\n}");
  EXPECT_TRUE(r.ok());
}

// ===========================================================================
// Increment / decrement
// ===========================================================================

TEST(TypeCheck, IncrementInt) {
  auto r = TC::from("fn f() {\n  x := 0\n  x++\n}");
  EXPECT_TRUE(r.ok());
}

TEST(TypeCheck, IncrementNonInt) {
  auto r = TC::from("fn f() {\n  x := 3.14\n  x++\n}");
  EXPECT_TRUE(r.has_err("increment requires integer"));
}

TEST(TypeCheck, DecrementInt) {
  auto r = TC::from("fn f() {\n  x := 5\n  x--\n}");
  EXPECT_TRUE(r.ok());
}

// ===========================================================================
// Function calls
// ===========================================================================

TEST(TypeCheck, CallCorrectArgs) {
  auto r = TC::from(
      "fn add(a, b Int) Int { a + b }\n"
      "fn f() Int { add(1, 2) }");
  EXPECT_TRUE(r.ok());
}

TEST(TypeCheck, CallWrongArgCount) {
  auto r = TC::from(
      "fn add(a, b Int) Int { a + b }\n"
      "fn f() { add(1) }");
  EXPECT_TRUE(r.has_err("expected 2 argument(s), got 1"));
}

TEST(TypeCheck, CallWrongArgType) {
  auto r = TC::from(
      "fn add(a, b Int) Int { a + b }\n"
      "fn f() { add(1, \"two\") }");
  EXPECT_TRUE(r.has_err("argument 2"));
}

TEST(TypeCheck, CallNonCallable) {
  auto r = TC::from("fn f() {\n  x := 42\n  x()\n}");
  EXPECT_TRUE(r.has_err("not callable"));
}

// ===========================================================================
// Indexing
// ===========================================================================

TEST(TypeCheck, ArrayIndexInt) {
  auto r = TC::from("fn f() {\n  arr := [1, 2, 3]\n  arr[0]\n}");
  EXPECT_TRUE(r.ok());
}

TEST(TypeCheck, ArrayIndexNonInt) {
  auto r = TC::from("fn f() {\n  arr := [1, 2]\n  arr[\"x\"]\n}");
  EXPECT_TRUE(r.has_err("array index must be an integer"));
}

TEST(TypeCheck, StringIndex) {
  auto r = TC::from("fn f() String {\n  s := \"hello\"\n  s[0]\n}");
  EXPECT_TRUE(r.ok());
}

// ===========================================================================
// Selectors
// ===========================================================================

TEST(TypeCheck, StructFieldAccess) {
  auto r = TC::from(
      "struct Point { x, y Int }\n"
      "fn f() Int {\n  p := Point{x: 1, y: 2}\n  p.x\n}");
  EXPECT_TRUE(r.ok());
}

TEST(TypeCheck, StructUnknownField) {
  auto r = TC::from(
      "struct Point { x, y Int }\n"
      "fn f() {\n  p := Point{x: 1, y: 2}\n  p.z\n}");
  EXPECT_TRUE(r.has_err("has no member 'z'"));
}

// ===========================================================================
// Struct literals
// ===========================================================================

TEST(TypeCheck, StructLiteralCorrect) {
  auto r = TC::from(
      "struct Point { x, y Int }\n"
      "fn f() Point { Point{x: 1, y: 2} }");
  EXPECT_TRUE(r.ok());
}

TEST(TypeCheck, StructLiteralWrongFieldType) {
  auto r = TC::from(
      "struct Point { x, y Int }\n"
      "fn f() { Point{x: \"oops\", y: 2} }");
  EXPECT_TRUE(r.has_err("field 'x'"));
}

TEST(TypeCheck, StructLiteralUnknownField) {
  auto r = TC::from(
      "struct Point { x, y Int }\n"
      "fn f() { Point{x: 1, z: 2} }");
  EXPECT_TRUE(r.has_err("has no field 'z'"));
}

// ===========================================================================
// Array literals
// ===========================================================================

TEST(TypeCheck, ArrayLiteralHomogeneous) {
  auto r = TC::from("fn f() { [1, 2, 3] }");
  EXPECT_TRUE(r.ok());
}

TEST(TypeCheck, ArrayLiteralMixed) {
  auto r = TC::from("fn f() { [1, \"two\"] }");
  EXPECT_TRUE(r.has_err("array element"));
}

// ===========================================================================
// Map literals
// ===========================================================================

TEST(TypeCheck, MapLiteralHomogeneous) {
  auto r = TC::from("fn f() { {\"a\": 1, \"b\": 2} }");
  EXPECT_TRUE(r.ok());
}

TEST(TypeCheck, MapLiteralMixedValues) {
  auto r = TC::from("fn f() { {\"a\": 1, \"b\": \"two\"} }");
  EXPECT_TRUE(r.has_err("map value"));
}

// ===========================================================================
// Range expressions
// ===========================================================================

TEST(TypeCheck, RangeExpr) {
  auto r = TC::from("fn f() { (0..10) }");
  EXPECT_TRUE(r.ok());
}

TEST(TypeCheck, RangeExprNonNumeric) {
  auto r = TC::from("fn f() { (\"a\"..\"z\") }");
  EXPECT_TRUE(r.has_err("range requires numeric type"));
}

// ===========================================================================
// For loops
// ===========================================================================

TEST(TypeCheck, ForRangeArray) {
  auto r = TC::from(
      "fn f() {\n"
      "  arr := [1, 2, 3]\n"
      "  for v : arr { v }\n"
      "}");
  EXPECT_TRUE(r.ok());
}

TEST(TypeCheck, ForRangeNonIterable) {
  auto r = TC::from("fn f() {\n  for v : 42 { v }\n}");
  EXPECT_TRUE(r.has_err("not iterable"));
}

TEST(TypeCheck, ForConditionBool) {
  auto r = TC::from("fn f() {\n  for true { break }\n}");
  EXPECT_TRUE(r.ok());
}

TEST(TypeCheck, ForConditionNonBool) {
  auto r = TC::from("fn f() {\n  for 42 { break }\n}");
  EXPECT_TRUE(r.has_err("condition"));
}

TEST(TypeCheck, ForIterClause) {
  auto r = TC::from(
      "fn f() {\n"
      "  for i := 0; i < 10; i += 1 {\n"
      "    i\n"
      "  }\n"
      "}");
  EXPECT_TRUE(r.ok());
}

// ===========================================================================
// Or expressions
// ===========================================================================

TEST(TypeCheck, OrExprStripsError) {
  // The or clause should work without errors on basic usage.
  auto r = TC::from(
      "fn f() {\n"
      "  x := 1\n"
      "  x or { 0 }\n"
      "}");
  EXPECT_TRUE(r.ok());
}

// ===========================================================================
// Spawn expressions
// ===========================================================================

TEST(TypeCheck, SpawnBlock) {
  auto r = TC::from("fn f() {\n  spawn { 42 }\n}");
  EXPECT_TRUE(r.ok());
}

// ===========================================================================
// Function expressions (closures)
// ===========================================================================

TEST(TypeCheck, FuncExpr) {
  auto r = TC::from(
      "fn f() {\n"
      "  add := fn(a, b Int) Int { a + b }\n"
      "  add(1, 2)\n"
      "}");
  EXPECT_TRUE(r.ok());
}

TEST(TypeCheck, FuncExprReturnMismatch) {
  auto r = TC::from(
      "fn f() {\n"
      "  bad := fn() Int { \"oops\" }\n"
      "}");
  EXPECT_TRUE(r.has_err("return type"));
}

// ===========================================================================
// Switch expressions
// ===========================================================================

TEST(TypeCheck, SwitchExpr) {
  auto r = TC::from(
      "fn f(x Int) Int {\n"
      "  switch x {\n"
      "    case 0: 0\n"
      "    case 1: 1\n"
      "    else: x\n"
      "  }\n"
      "}");
  EXPECT_TRUE(r.ok());
}

// ===========================================================================
// Const declarations
// ===========================================================================

TEST(TypeCheck, ConstDeclMatchesType) {
  auto r = TC::from("const Pi Float = 3.14");
  EXPECT_TRUE(r.ok());
}

TEST(TypeCheck, ConstDeclTypeMismatch) {
  auto r = TC::from("const Pi Int = 3.14");
  EXPECT_TRUE(r.has_err("constant initializer"));
}

// ===========================================================================
// Struct duplicate checking
// ===========================================================================

TEST(TypeCheck, StructDuplicateField) {
  auto r = TC::from("struct Bad {\n  x Int\n  x String\n}");
  EXPECT_TRUE(r.has_err("duplicate field 'x'"));
}

// ===========================================================================
// Int → Float promotion
// ===========================================================================

TEST(TypeCheck, IntToFloatPromotion) {
  auto r = TC::from("fn f() Float { 42 }");
  EXPECT_TRUE(r.ok());
}

TEST(TypeCheck, FloatToIntNoPromotion) {
  auto r = TC::from("fn f() Int { 3.14 }");
  EXPECT_TRUE(r.has_err("return type"));
}

// ===========================================================================
// Bitwise operators
// ===========================================================================

TEST(TypeCheck, BitwiseAnd) {
  auto r = TC::from("fn f() Int { 0xFF & 0x0F }");
  EXPECT_TRUE(r.ok());
}

TEST(TypeCheck, BitwiseOnFloat) {
  auto r = TC::from("fn f() { 1.0 & 2.0 }");
  EXPECT_TRUE(r.has_err("bitwise operator requires integer type"));
}

// ===========================================================================
// Embedded struct member access
// ===========================================================================

TEST(TypeCheck, EmbeddedFieldAccess) {
  auto r = TC::from(
      "struct Base { x Int }\n"
      "struct Child < Base { y Int }\n"
      "fn f() Int {\n  c := Child{x: 1, y: 2}\n  c.x\n}");
  EXPECT_TRUE(r.ok());
}

// ===========================================================================
// Receiver methods
// ===========================================================================

TEST(TypeCheck, ReceiverMethodAccess) {
  auto r = TC::from(
      "struct Counter { n Int }\n"
      "fn (c Counter) Value() Int { c.n }\n"
      "fn f() Int {\n  c := Counter{n: 42}\n  c.Value()\n}");
  EXPECT_TRUE(r.ok());
}

TEST(TypeCheck, ArrayGenericReceiverMethod) {
  // stdlib-mode array receiver with generic T; Len() returns Int.
  auto r = TC::from(
      "fn |T| (self T[]) Len() Int { 0 }\n"
      "fn f() Int {\n  arr := [1, 2, 3]\n  arr.Len()\n}");
  EXPECT_TRUE(r.ok());
}

TEST(TypeCheck, ArrayGenericReceiverMethodSubstitution) {
  // Return type T[] should substitute T→Int for Int[] receiver.
  auto r = TC::from(
      "fn |T| (self T[]) Clone() T[] { self }\n"
      "fn f() Int[] {\n  arr := [1, 2, 3]\n  arr.Clone()\n}");
  EXPECT_TRUE(r.ok());
}

TEST(TypeCheck, ArrayGenericReceiverMethodNonStdlibRejected) {
  auto r = TC::from("fn |T| (self T[]) Len() Int { 0 }",
                     /*stdlib=*/false);
  EXPECT_TRUE(r.has_err("receiver methods on generic types can only be "
                         "defined in stdlib packages"));
}

TEST(TypeCheck, MapGenericReceiverMethod) {
  // stdlib-mode map receiver with generic K, V; Size() returns Int.
  auto r = TC::from(
      "fn |K, V| (self {K:V}) Size() Int { 0 }\n"
      "fn f() Int {\n  m := {\"a\": 1}\n  m.Size()\n}");
  EXPECT_TRUE(r.ok());
}

// ===========================================================================
// Bitwise NOT (~)
// ===========================================================================

TEST(TypeCheck, BitwiseNotOnInt) {
  auto r = TC::from("fn f() Int {\n  x := 42\n  ~x\n}");
  EXPECT_TRUE(r.ok());
}

TEST(TypeCheck, BitwiseNotOnNonInt) {
  auto r = TC::from("fn f() {\n  x := 3.14\n  ~x\n}");
  EXPECT_TRUE(r.has_err("bitwise NOT requires integer type"));
}

TEST(TypeCheck, BitwiseNotOnBool) {
  auto r = TC::from("fn f() {\n  x := true\n  ~x\n}");
  EXPECT_TRUE(r.has_err("bitwise NOT requires integer type"));
}

// ===========================================================================
// Compound assignment operators
// ===========================================================================

TEST(TypeCheck, SubAssignmentNumeric) {
  auto r = TC::from("fn f() {\n  x := 10\n  x -= 3\n}");
  EXPECT_TRUE(r.ok());
}

TEST(TypeCheck, MulAssignmentNumeric) {
  auto r = TC::from("fn f() {\n  x := 10\n  x *= 2\n}");
  EXPECT_TRUE(r.ok());
}

TEST(TypeCheck, DivAssignmentNumeric) {
  auto r = TC::from("fn f() {\n  x := 10\n  x /= 2\n}");
  EXPECT_TRUE(r.ok());
}

TEST(TypeCheck, SubAssignmentOnString) {
  auto r = TC::from("fn f() {\n  x := \"hello\"\n  x -= \"h\"\n}");
  EXPECT_TRUE(r.has_err("compound assignment requires numeric type"));
}

TEST(TypeCheck, DivAssignmentOnString) {
  auto r = TC::from("fn f() {\n  x := \"hello\"\n  x /= \"h\"\n}");
  EXPECT_TRUE(r.has_err("/= requires numeric type"));
}

// ===========================================================================
// Char builtin type
// ===========================================================================

TEST(TypeCheck, CharTypeExists) {
  // Char is a recognized builtin type and can be declared.
  auto r = TC::from("fn f() {\n  c Char\n}");
  EXPECT_TRUE(r.ok()) << "Char should be a recognized type";
}

TEST(TypeCheck, CharNotDirectlyAssignableFromInt) {
  // A *typed* Int variable is not directly assignable to Char — even
  // though both are integer kinds, narrowing requires a conversion.
  // (A bare integer literal is "untyped" and may flow into any integer
  // width; that case is exercised separately.)
  auto r = TC::from("fn f() {\n  x := 65\n  c Char = x\n}");
  EXPECT_FALSE(r.ok()) << "typed Int should not be directly assignable to Char";
  EXPECT_TRUE(r.has_err("variable initializer"));
}

TEST(TypeCheck, IntCharConversion) {
  auto r = TC::from("fn f() {\n  x := 65\n  x.Char()\n}");
  EXPECT_TRUE(r.ok()) << "Int should have a .Char() method";
}

// ===========================================================================
// Untyped integer literal coercion (Phase 6a)
// ===========================================================================

TEST(TypeCheck, IntLiteralAssignsToNarrowInt) {
  // Bare literal flows into a narrow Int annotation.
  auto r = TC::from("fn f() {\n  x Int64 = 5\n  y Int32 = 7\n  z Int8 = 1\n}");
  EXPECT_TRUE(r.ok());
}

TEST(TypeCheck, IntLiteralPassesAsNarrowParam) {
  auto r = TC::from(
      "fn Take(n Int32) {}\n"
      "fn f() { Take(7) }");
  EXPECT_TRUE(r.ok());
}

TEST(TypeCheck, IntLiteralBinaryOpWithNarrowInt) {
  // `x + 5` keeps x's narrow Int width when 5 is an untyped literal.
  auto r = TC::from(
      "fn f() {\n  x Int32 = 1\n  y Int32 = x + 5\n}");
  EXPECT_TRUE(r.ok());
}

TEST(TypeCheck, IntLiteralInStructFieldOfNarrowInt) {
  auto r = TC::from(
      "struct Header { len Int32 }\n"
      "fn f() { h := Header{len: 42} }");
  EXPECT_TRUE(r.ok());
}

TEST(TypeCheck, TypedIntNotAssignableToNarrowInt) {
  // Without an explicit annotation `:=` materializes to plain Int, which
  // is then *not* implicitly assignable to a narrower width.
  auto r = TC::from("fn f() {\n  x := 5\n  y Int32 = x\n}");
  EXPECT_FALSE(r.ok());
  EXPECT_TRUE(r.has_err("variable initializer"));
}

// ===========================================================================
// In-bound struct methods — field access by name
// ===========================================================================

TEST(TypeCheck, InBoundMethodAccessesField) {
  auto r = TC::from(
      "struct Dog {\n"
      "  name String\n"
      "  pub fn Speak() String { name }\n"
      "}");
  EXPECT_TRUE(r.ok()) << "In-bound method should access struct fields by name";
}

TEST(TypeCheck, InBoundMethodAccessesMultipleFields) {
  auto r = TC::from(
      "struct Point {\n"
      "  x, y Int\n"
      "  pub fn Sum() Int { x + y }\n"
      "}");
  EXPECT_TRUE(r.ok());
}

TEST(TypeCheck, InBoundMethodWithParams) {
  auto r = TC::from(
      "struct Counter {\n"
      "  n Int\n"
      "  pub fn Add(x Int) Int { n + x }\n"
      "}");
  EXPECT_TRUE(r.ok());
}

TEST(TypeCheck, InBoundMethodFieldTypeChecked) {
  // Using a field in an incompatible way should still produce a type error.
  auto r = TC::from(
      "struct Foo {\n"
      "  name String\n"
      "  pub fn Bad() Int { name }\n"
      "}");
  EXPECT_FALSE(r.ok());
  EXPECT_TRUE(r.has_err("return type"));
}

TEST(TypeCheck, InBoundMethodUndefinedFieldStillErrors) {
  // Accessing a name that isn't a field should still error.
  auto r = TC::from(
      "struct Foo {\n"
      "  x Int\n"
      "  pub fn Bad() Int { unknown }\n"
      "}");
  EXPECT_FALSE(r.ok());
  EXPECT_TRUE(r.has_err("undefined"));
}

TEST(TypeCheck, InBoundMethodDoesNotLeakFields) {
  // Fields should not be visible outside the struct's methods.
  auto r = TC::from(
      "struct Foo {\n"
      "  x Int\n"
      "  pub fn Get() Int { x }\n"
      "}\n"
      "fn f() Int { x }");
  EXPECT_FALSE(r.ok());
  EXPECT_TRUE(r.has_err("undefined"));
}

// ===========================================================================
// Interface method resolution
// ===========================================================================

TEST(TypeCheck, InterfaceMethodCall) {
  auto r = TC::from(
      "interface Speaker { Speak() String }\n"
      "struct Dog { name String }\n"
      "fn (d Dog) Speak() String { d.name }\n"
      "fn f(s Speaker) String { s.Speak() }");
  EXPECT_TRUE(r.ok()) << "Should resolve method Speak() on interface Speaker";
}

TEST(TypeCheck, InterfaceMethodCallReturnType) {
  // The return type of the interface method should be used for the expression.
  auto r = TC::from(
      "interface Speaker { Speak() String }\n"
      "fn f(s Speaker) String { s.Speak() }");
  EXPECT_TRUE(r.ok());
}

TEST(TypeCheck, InterfaceMethodUnknown) {
  auto r = TC::from(
      "interface Speaker { Speak() String }\n"
      "fn f(s Speaker) Void { s.Bark() }");
  EXPECT_FALSE(r.ok());
  EXPECT_TRUE(r.has_err("no member"));
}

TEST(TypeCheck, InterfaceAssignConcreteType) {
  auto r = TC::from(
      "interface Speaker { Speak() String }\n"
      "struct Dog { name String }\n"
      "fn (d Dog) Speak() String { d.name }\n"
      "fn f() Void {\n"
      "  d := Dog{name: \"Rex\"}\n"
      "  s Speaker = d\n"
      "}");
  EXPECT_TRUE(r.ok()) << "Concrete struct satisfying interface should be assignable";
}

TEST(TypeCheck, InterfaceAssignNonConforming) {
  auto r = TC::from(
      "interface Speaker { Speak() String }\n"
      "struct Cat { name String }\n"
      "fn f() Void {\n"
      "  c := Cat{name: \"Mittens\"}\n"
      "  s Speaker = c\n"
      "}");
  EXPECT_FALSE(r.ok()) << "Cat does not implement Speak(), should error";
}

TEST(TypeCheck, InterfaceMultipleMethods) {
  auto r = TC::from(
      "interface ReadWriter {\n"
      "  Read() String\n"
      "  Write(s String) Void\n"
      "}\n"
      "fn f(rw ReadWriter) Void {\n"
      "  x := rw.Read()\n"
      "  rw.Write(x)\n"
      "}");
  EXPECT_TRUE(r.ok());
}

TEST(TypeCheck, InterfaceMethodCallInExpression) {
  auto r = TC::from(
      "interface Sizer { Size() Int }\n"
      "fn f(s Sizer) Int { s.Size() + 1 }");
  EXPECT_TRUE(r.ok());
}

// ===========================================================================
// Return inside branching constructs (if/else, switch)
// ===========================================================================

TEST(TypeCheck, ReturnInIfElseBranches) {
  auto r = TC::from(
      "fn f(x Int) Int {\n"
      "  if x > 0 {\n"
      "    return 1\n"
      "  } else {\n"
      "    return -1\n"
      "  }\n"
      "}");
  EXPECT_TRUE(r.ok())
      << "if/else where both branches return should satisfy return type";
}

TEST(TypeCheck, ReturnInSwitchAllArms) {
  auto r = TC::from(
      "fn f(x Int) Int {\n"
      "  switch x {\n"
      "    case 0: {\n"
      "      return 100\n"
      "    }\n"
      "    case 1: {\n"
      "      return 200\n"
      "    }\n"
      "    else: {\n"
      "      return 300\n"
      "    }\n"
      "  }\n"
      "}");
  EXPECT_TRUE(r.ok())
      << "switch where all arms return should satisfy return type";
}

TEST(TypeCheck, ReturnInNestedIfInsideSwitch) {
  auto r = TC::from(
      "fn f(x Int) Int {\n"
      "  switch x {\n"
      "    case 0: {\n"
      "      if x == 0 {\n"
      "        return 10\n"
      "      } else {\n"
      "        return 20\n"
      "      }\n"
      "    }\n"
      "    else: {\n"
      "      return 30\n"
      "    }\n"
      "  }\n"
      "}");
  EXPECT_TRUE(r.ok())
      << "nested if/else inside switch arms should be recognized";
}

TEST(TypeCheck, ReturnInIfWithoutElseStillErrors) {
  // If there's no else, the function might not return.
  auto r = TC::from(
      "fn f(x Int) Int {\n"
      "  if x > 0 {\n"
      "    return 1\n"
      "  }\n"
      "}");
  EXPECT_FALSE(r.ok())
      << "if without else doesn't guarantee a return";
}

TEST(TypeCheck, ReturnInSwitchWithoutElseStillErrors) {
  auto r = TC::from(
      "fn f(x Int) Int {\n"
      "  switch x {\n"
      "    case 0: {\n"
      "      return 100\n"
      "    }\n"
      "  }\n"
      "}");
  EXPECT_FALSE(r.ok())
      << "switch without else doesn't guarantee a return";
}

TEST(TypeCheck, ReturnInSwitchPartialArmStillErrors) {
  // One arm doesn't return.
  auto r = TC::from(
      "fn f(x Int) Int {\n"
      "  switch x {\n"
      "    case 0: {\n"
      "      return 100\n"
      "    }\n"
      "    case 1: {\n"
      "      intrinsic_print(\"hi\")\n"
      "    }\n"
      "    else: {\n"
      "      return 300\n"
      "    }\n"
      "  }\n"
      "}");
  EXPECT_FALSE(r.ok())
      << "switch with a non-returning arm doesn't guarantee a return";
}

TEST(TypeCheck, MixedReturnAndTailValue) {
  // Some branches return, last one uses tail expression — should still work.
  auto r = TC::from(
      "fn f(x Int) Int {\n"
      "  if x > 0 {\n"
      "    return 1\n"
      "  }\n"
      "  0\n"
      "}");
  EXPECT_TRUE(r.ok())
      << "early return + tail value should be fine";
}

// ===========================================================================
// Union types and or-clause
// ===========================================================================

TEST(TypeCheck, UnionTypeVarDecl) {
  auto r = TC::from(
      "fn f() {\n"
      "  x Int | Error = 0\n"
      "}");
  EXPECT_TRUE(r.ok());
}

TEST(TypeCheck, DivisionOrExprStripsToInt) {
  // Division returns Int | Error, or should strip to Int.
  auto r = TC::from(
      "fn f() Int {\n"
      "  10 / 2 or { 0 }\n"
      "}");
  EXPECT_TRUE(r.ok());
}

TEST(TypeCheck, OrExprWithPipeVariable) {
  auto r = TC::from(
      "fn f() Int {\n"
      "  10 / 2 or |err| { 0 }\n"
      "}");
  EXPECT_TRUE(r.ok());
}

TEST(TypeCheck, OrExprEmptyBlock) {
  // Empty or block returns zero value of the type.
  auto r = TC::from(
      "fn f() {\n"
      "  x := 10 / 2 or {}\n"
      "}");
  EXPECT_TRUE(r.ok());
}

TEST(TypeCheck, OrExprOnNonUnionPassesThrough) {
  // Using or on a non-union type should not error (it's a no-op).
  auto r = TC::from(
      "fn f() {\n"
      "  x := 42\n"
      "  x or { 0 }\n"
      "}");
  EXPECT_TRUE(r.ok());
}

TEST(TypeCheck, PureUnionType) {
  // Bool | Int is a pure union (no Error).
  auto r = TC::from(
      "fn f() {\n"
      "  x Bool | Int = 0\n"
      "}");
  EXPECT_TRUE(r.ok());
}

TEST(TypeCheck, UnionTypeAssignString) {
  // Assigning a String to an Int | String union should work.
  auto r = TC::from(
      "fn f() {\n"
      "  x Int | String = \"hello\"\n"
      "}");
  EXPECT_TRUE(r.ok());
}

TEST(TypeCheck, OrExprStripsManyToUnion) {
  // Int | String | Error — or should strip Error, leaving Int | String.
  // We can't easily construct this without a function that returns it,
  // but we can verify the general analysis doesn't crash.
  auto r = TC::from(
      "fn f() {\n"
      "  x := 10 / 2\n"
      "  y := x or { 0 }\n"
      "}");
  EXPECT_TRUE(r.ok());
}

// ===========================================================================
// Type matching on unions
// ===========================================================================

TEST(TypeCheck, IfTypeMatchNarrows) {
  // Type matching in if should narrow the variable.
  auto r = TC::from(
      "fn f() {\n"
      "  x Int | Error = 0\n"
      "  if x == Int {\n"
      "    y := x + 1\n"
      "  }\n"
      "}");
  EXPECT_TRUE(r.ok());
}

TEST(TypeCheck, SwitchTypeMatch) {
  // Type matching in switch on a union.
  auto r = TC::from(
      "fn f() {\n"
      "  x Int | String = 0\n"
      "  switch x {\n"
      "    case Int: { 0 }\n"
      "    case String: { 1 }\n"
      "  }\n"
      "}");
  EXPECT_TRUE(r.ok());
}

// Spec: `const X = A | B` is a type-union alias when A and B resolve
// to types.  (docs/language.md:945-948)
TEST(TypeCheck, ConstDecl_UnionOfInterfaces_IsTypeAlias) {
  auto r = TC::from(
      "interface Reader { Read() String }\n"
      "interface Writer { Write(s String) Void }\n"
      "const ReadWriter = Reader | Writer\n"
      "struct RW {\n"
      "  pub fn Read() String { \"\" }\n"
      "  pub fn Write(s String) Void {}\n"
      "}\n"
      "fn use(rw ReadWriter) Void {}\n"
      "fn f() Void { use(RW{}) }");
  EXPECT_TRUE(r.ok());
}

// Spec: arrays of the same element type pass directly into a variadic.
// (docs/language.md:276-285)
TEST(TypeCheck, Variadic_AcceptsArrayPassthrough) {
  auto r = TC::from(
      "fn Sum(args ...Int) Int { 0 }\n"
      "fn f() Int { Sum([1, 2, 3]) }");
  EXPECT_TRUE(r.ok());
}

TEST(TypeCheck, Variadic_AcceptsScalarArgs) {
  auto r = TC::from(
      "fn Sum(args ...Int) Int { 0 }\n"
      "fn f() Int { Sum(1, 2, 3) }");
  EXPECT_TRUE(r.ok());
}

TEST(TypeCheck, Variadic_RejectsMixedArrayAndScalars) {
  // Spec: "you can't mix and match" (line 286-287).
  auto r = TC::from(
      "fn Sum(args ...Int) Int { 0 }\n"
      "fn f() Int { Sum(1, 2, [3, 4]) }");
  EXPECT_TRUE(r.has_err("variadic argument"));
}

TEST(TypeCheck, SwitchTypeMatch_NonExhaustive_Rejected) {
  // Spec: type matches must be exhaustive without `else`.
  // (docs/language.md:1174-1177)
  auto r = TC::from(
      "fn f() {\n"
      "  x Int | String | Bool = 0\n"
      "  switch x {\n"
      "    case Int: { 0 }\n"
      "  }\n"
      "}");
  EXPECT_TRUE(r.has_err("non-exhaustive type-switch"));
}

TEST(TypeCheck, SwitchTypeMatch_WithElse_NotExhaustiveOk) {
  auto r = TC::from(
      "fn f() {\n"
      "  x Int | String | Bool = 0\n"
      "  switch x {\n"
      "    case Int: { 0 }\n"
      "    else: { 1 }\n"
      "  }\n"
      "}");
  EXPECT_TRUE(r.ok());
}

TEST(TypeCheck, IfTypeMatchWithElse) {
  // Type matching with else should narrow the else branch too.
  auto r = TC::from(
      "fn f() {\n"
      "  x Int | String = 0\n"
      "  if x == Int {\n"
      "    y := x + 1\n"
      "  } else {\n"
      "    z := x\n"
      "  }\n"
      "}");
  EXPECT_TRUE(r.ok());
}

// ===========================================================================
// Intrinsic type checking
// ===========================================================================

TEST(TypeCheck, IntrinsicYieldAcceptsNoArgs) {
  auto r = TC::from(
      "fn f() {\n"
      "  intrinsic_yield()\n"
      "}");
  EXPECT_TRUE(r.ok());
}

TEST(TypeCheck, IntrinsicTrapAcceptsString) {
  auto r = TC::from(
      "fn f() {\n"
      "  intrinsic_trap(\"reason\")\n"
      "}");
  EXPECT_TRUE(r.ok());
}

TEST(TypeCheck, IntrinsicAtomicAddAcceptsTwoInts) {
  auto r = TC::from(
      "fn f() {\n"
      "  x := 0\n"
      "  old := intrinsic_atomic_add(x, 1)\n"
      "}");
  EXPECT_TRUE(r.ok());
}

TEST(TypeCheck, IntrinsicYieldInsideSpawn) {
  auto r = TC::from(
      "fn f() {\n"
      "  spawn |ctx| {\n"
      "    intrinsic_yield()\n"
      "    42\n"
      "  }\n"
      "}");
  EXPECT_TRUE(r.ok());
}

TEST(TypeCheck, IntrinsicTrapInsideSpawn) {
  auto r = TC::from(
      "fn f() {\n"
      "  spawn |ctx| {\n"
      "    intrinsic_trap(\"fatal\")\n"
      "    42\n"
      "  }\n"
      "}");
  EXPECT_TRUE(r.ok());
}

// ===========================================================================
// Operator overloading — struct types implementing operator interfaces
// ===========================================================================

// Minimal structs used per-test to avoid cascading analysis errors.
// Each source string is self-contained.

TEST(TypeCheck, StructOperatorAdd) {
  auto r = TC::from(
      "struct V { n Int\n"
      "  pub fn Add(other V) V { V{n: n + other.n} }\n"
      "}\n"
      "fn f(a V, b V) V { a + b }\n");
  EXPECT_TRUE(r.ok()) << "V + V should resolve to Add method";
}

TEST(TypeCheck, StructOperatorSub) {
  auto r = TC::from(
      "struct V { n Int\n"
      "  pub fn Sub(other V) V { V{n: n - other.n} }\n"
      "}\n"
      "fn f(a V, b V) V { a - b }\n");
  EXPECT_TRUE(r.ok()) << "V - V should resolve to Sub method";
}

TEST(TypeCheck, StructOperatorMul) {
  auto r = TC::from(
      "struct V { n Int\n"
      "  pub fn Mul(other V) V { V{n: n * other.n} }\n"
      "}\n"
      "fn f(a V, b V) V { a * b }\n");
  EXPECT_TRUE(r.ok()) << "V * V should resolve to Mul method";
}

TEST(TypeCheck, StructOperatorDiv) {
  // Div returns V | Error per the Divisable interface.
  auto r = TC::from(
      "struct V { n Int\n"
      "  pub fn Div(other V) V | Error { V{n: n} }\n"
      "}\n"
      "fn f(a V, b V) V | Error { a / b }\n");
  EXPECT_TRUE(r.ok()) << "V / V should resolve to Div method (returns T|Error)";
}

TEST(TypeCheck, StructOperatorEqual) {
  auto r = TC::from(
      "struct V { n Int\n"
      "  pub fn Equals(other V) Bool { n == other.n }\n"
      "}\n"
      "fn f(a V, b V) Bool { a == b }\n");
  EXPECT_TRUE(r.ok()) << "V == V should resolve to Equals method";
}

TEST(TypeCheck, StructOperatorNotEqual) {
  auto r = TC::from(
      "struct V { n Int\n"
      "  pub fn Equals(other V) Bool { n == other.n }\n"
      "}\n"
      "fn f(a V, b V) Bool { a != b }\n");
  EXPECT_TRUE(r.ok()) << "V != V should resolve to Equals method (negated)";
}

TEST(TypeCheck, StructOperatorLessThan) {
  auto r = TC::from(
      "struct V { n Int\n"
      "  pub fn Compare(other V) Comparison { n.Compare(other.n) }\n"
      "}\n"
      "fn f(a V, b V) Bool { a < b }\n");
  EXPECT_TRUE(r.ok()) << "V < V should resolve to Compare method";
}

TEST(TypeCheck, StructOperatorGreaterThan) {
  auto r = TC::from(
      "struct V { n Int\n"
      "  pub fn Compare(other V) Comparison { n.Compare(other.n) }\n"
      "}\n"
      "fn f(a V, b V) Bool { a > b }\n");
  EXPECT_TRUE(r.ok()) << "V > V should resolve to Compare method";
}

TEST(TypeCheck, StructOperatorLessThanEqual) {
  auto r = TC::from(
      "struct V { n Int\n"
      "  pub fn Compare(other V) Comparison { n.Compare(other.n) }\n"
      "}\n"
      "fn f(a V, b V) Bool { a <= b }\n");
  EXPECT_TRUE(r.ok()) << "V <= V should resolve to Compare method";
}

TEST(TypeCheck, StructOperatorGreaterThanEqual) {
  auto r = TC::from(
      "struct V { n Int\n"
      "  pub fn Compare(other V) Comparison { n.Compare(other.n) }\n"
      "}\n"
      "fn f(a V, b V) Bool { a >= b }\n");
  EXPECT_TRUE(r.ok()) << "V >= V should resolve to Compare method";
}

// Fallback: type has only Compare, no Equals — == and != fall back via Compare.
TEST(TypeCheck, StructEqualFallsBackToCompare) {
  auto r = TC::from(
      "struct V { n Int\n"
      "  pub fn Compare(other V) Comparison { n.Compare(other.n) }\n"
      "}\n"
      "fn f(a V, b V) Bool { a == b }\n");
  EXPECT_TRUE(r.ok())
      << "== should fall back to Compare when Equals is absent";
}

TEST(TypeCheck, StructNotEqualFallsBackToCompare) {
  auto r = TC::from(
      "struct V { n Int\n"
      "  pub fn Compare(other V) Comparison { n.Compare(other.n) }\n"
      "}\n"
      "fn f(a V, b V) Bool { a != b }\n");
  EXPECT_TRUE(r.ok())
      << "!= should fall back to Compare when Equals is absent";
}

// Error cases: struct with no operator methods.
TEST(TypeCheck, StructAddMissingMethod) {
  auto r = TC::from(
      "struct Bare { n Int }\n"
      "fn f(a Bare, b Bare) Bare { a + b }\n");
  EXPECT_TRUE(r.has_err("does not implement Adder"));
}

TEST(TypeCheck, StructCompareMissingMethod) {
  auto r = TC::from(
      "struct Bare { n Int }\n"
      "fn f(a Bare, b Bare) Bool { a < b }\n");
  EXPECT_TRUE(r.has_err("does not implement Comparable"));
}

TEST(TypeCheck, StructEqualityMissingMethod) {
  auto r = TC::from(
      "struct Bare { n Int }\n"
      "fn f(a Bare, b Bare) Bool { a == b }\n");
  EXPECT_TRUE(r.has_err("does not support equality"));
}

// struct_operator_methods table must be populated so codegen can use it.
TEST(TypeCheck, StructOperatorMethodTablePopulated) {
  auto r = TC::from(
      "struct V { n Int\n"
      "  pub fn Add(other V) V { V{n: n + other.n} }\n"
      "}\n"
      "fn f(a V, b V) V { a + b }\n");
  ASSERT_TRUE(r.ok());
  // Exactly one binary expression was resolved to a struct method.
  EXPECT_EQ(r.analyzer->struct_operator_methods.size(), 1u);
  auto it = r.analyzer->struct_operator_methods.begin();
  EXPECT_EQ(it->second, "Add");
}

// ===========================================================================
// Iterable protocol — user structs implementing Next() T | Error
// ===========================================================================

TEST(TypeCheck, IterableStructBasic) {
  // A counter that yields Int values until exhausted.
  auto r = TC::from(
      "struct Counter {\n"
      "  n, limit Int\n"
      "  pub fn Next() Int | Error {\n"
      "    if n >= limit { return Missing{} }\n"
      "    n++\n"
      "    n - 1\n"
      "  }\n"
      "}\n"
      "fn f(c Counter) Void {\n"
      "  for v : c { intrinsic_print(v.String()) }\n"
      "}\n");
  EXPECT_TRUE(r.ok())
      << "Struct with Next() T | Error should be usable in a for loop";
}

TEST(TypeCheck, IterableStructElemTypeInferred) {
  // The loop variable type must be inferred as T from Next() T | Error.
  auto r = TC::from(
      "struct Src {\n"
      "  val Int\n"
      "  pub fn Next() String | Error { \"hi\" }\n"
      "}\n"
      "fn f(s Src) String {\n"
      "  for v : s { return v }\n"
      "  \"\"\n"
      "}\n");
  EXPECT_TRUE(r.ok())
      << "Loop variable should have type String (from Next() String | Error)";
}

TEST(TypeCheck, IterableStructElemTypeUsable) {
  // The loop variable should be usable as its inferred type (Int here).
  auto r = TC::from(
      "struct Src {\n"
      "  pub fn Next() Int | Error { 0 }\n"
      "}\n"
      "fn g(n Int) Void { }\n"
      "fn f(s Src) Void {\n"
      "  for v : s { g(v) }\n"
      "}\n");
  EXPECT_TRUE(r.ok())
      << "Loop variable has element type; passing to fn(Int) should work";
}

TEST(TypeCheck, NonIterableStructInForLoop) {
  // A plain struct with no Next() method should produce an error.
  auto r = TC::from(
      "struct Bare { n Int }\n"
      "fn f(b Bare) Void { for v : b { } }\n");
  EXPECT_TRUE(r.has_err("is not iterable"));
}

TEST(TypeCheck, IterableNextReturnsWrongType) {
  // Next() must return T | Error; returning plain T should NOT make it iterable.
  auto r = TC::from(
      "struct Src {\n"
      "  pub fn Next() Int { 0 }\n"
      "}\n"
      "fn f(s Src) Void { for v : s { } }\n");
  EXPECT_TRUE(r.has_err("is not iterable"));
}

TEST(TypeCheck, IterableStructRecordedInAnalyzer) {
  // The iterable_next_elem_type map must be populated for the codegen.
  auto r = TC::from(
      "struct Counter {\n"
      "  n Int\n"
      "  pub fn Next() Int | Error { n }\n"
      "}\n"
      "fn f(c Counter) Void { for v : c { } }\n");
  ASSERT_TRUE(r.ok());
  EXPECT_EQ(r.analyzer->iterable_next_elem_type.size(), 1u);
  auto it = r.analyzer->iterable_next_elem_type.begin();
  ASSERT_TRUE(it->second != nullptr);
  EXPECT_EQ(it->second->kind, TypeKind::Int);
}

// ===========================================================================
// Type alias methods — binding methods to any type via receiver syntax
// ===========================================================================

TEST(TypeCheck, TypeAliasMethodOnInt) {
  auto r = TC::from(
      "const UserID = Int\n"
      "pub fn (u UserID) Validate() Bool { true }\n"
      "fn f() Bool {\n"
      "  id UserID\n"
      "  id.Validate()\n"
      "}");
  EXPECT_TRUE(r.ok());
}

TEST(TypeCheck, TypeAliasInheritsBuiltinMethods) {
  auto r = TC::from(
      "const Name = String\n"
      "fn f() String {\n"
      "  n Name\n"
      "  n.Upper()\n"
      "}");
  EXPECT_TRUE(r.ok());
}

TEST(TypeCheck, TypeAliasOnStructInheritsFields) {
  auto r = TC::from(
      "struct Point { pub x, y Int }\n"
      "const MyPoint = Point\n"
      "pub fn (p MyPoint) Sum() Int { p.x }\n"
      "fn f() Int {\n"
      "  p := MyPoint{x: 1, y: 2}\n"
      "  p.Sum()\n"
      "}");
  EXPECT_TRUE(r.ok());
}

TEST(TypeCheck, TypeAliasMethodAndBuiltinCoexist) {
  auto r = TC::from(
      "const UserID = Int\n"
      "pub fn (u UserID) Label() String { \"user\" }\n"
      "fn f() {\n"
      "  id UserID\n"
      "  id.Label()\n"
      "  id.String()\n"
      "}");
  EXPECT_TRUE(r.ok());
}

TEST(TypeCheck, TypeAliasIsResolvedAsType) {
  // A type alias should be resolved as a Type symbol, so it can be used
  // in variable declarations.
  auto r = TC::from(
      "const UserID = Int\n"
      "fn f() {\n"
      "  id UserID\n"
      "}");
  EXPECT_TRUE(r.ok());
}

TEST(TypeCheck, TypeAliasMethodOnFloat) {
  auto r = TC::from(
      "const Temperature = Float\n"
      "pub fn (t Temperature) Celsius() Temperature { t }\n"
      "fn f() Temperature {\n"
      "  temp Temperature\n"
      "  temp.Celsius()\n"
      "}");
  EXPECT_TRUE(r.ok());
}

TEST(TypeCheck, TypeAliasMethodOnBool) {
  auto r = TC::from(
      "const Flag = Bool\n"
      "pub fn (fl Flag) IsSet() Bool { true }\n"
      "fn test() Bool {\n"
      "  active Flag\n"
      "  active.IsSet()\n"
      "}");
  EXPECT_TRUE(r.ok());
}

TEST(TypeCheck, TypeAliasMultipleMethods) {
  auto r = TC::from(
      "const ID = Int\n"
      "pub fn (i ID) IsValid() Bool { true }\n"
      "pub fn (i ID) Label() String { \"id\" }\n"
      "fn f() {\n"
      "  id ID\n"
      "  id.IsValid()\n"
      "  id.Label()\n"
      "}");
  EXPECT_TRUE(r.ok());
}

} // namespace saga

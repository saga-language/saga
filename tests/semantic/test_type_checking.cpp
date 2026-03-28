// Copywrite 2026 Rob Thornton
// SPDX-License-Identifier: MIT

#include "semantic/analyzer.hpp"
#include "frontend/file.hpp"
#include "frontend/fileset.hpp"
#include "frontend/parser.hpp"

#include <gtest/gtest.h>

namespace mc {

// ---------------------------------------------------------------------------
// Helper
// ---------------------------------------------------------------------------

struct TC {
  FileSet fileset;
  NodePtr ast;
  std::unique_ptr<Analyzer> analyzer;

  static TC from(const std::string &source) {
    TC r;
    r.fileset.add_file(File::from_source("test.sg", source));
    Parser parser(r.fileset);
    r.ast = parser.parse();
    r.analyzer = std::make_unique<Analyzer>(r.fileset);
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
  // Int and Char are different integer types (signed vs unsigned, different
  // bit widths).  Per the spec, types must be converted, not cast.
  auto r = TC::from("fn f() {\n  c Char = 65\n}");
  EXPECT_FALSE(r.ok()) << "Int should not be directly assignable to Char";
  EXPECT_TRUE(r.has_err("variable initializer"));
}

TEST(TypeCheck, IntCharConversion) {
  auto r = TC::from("fn f() {\n  x := 65\n  x.Char()\n}");
  EXPECT_TRUE(r.ok()) << "Int should have a .Char() method";
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

} // namespace mc

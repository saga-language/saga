// Copywrite 2026 Rob Thornton
// SPDX-License-Identifier: MIT

#include "semantic/analyzer.hpp"
#include "frontend/file.hpp"
#include "frontend/fileset.hpp"
#include "frontend/parser.hpp"

#include <gtest/gtest.h>

#include <set>
#include <string>

namespace saga {

// ---------------------------------------------------------------------------
// Helper — parse source text and run the analyzer, returning both.
// ---------------------------------------------------------------------------

struct AnalysisResult {
  FileSet fileset;
  NodePtr ast;
  std::unique_ptr<Analyzer> analyzer;

  static AnalysisResult from(const std::string &source, bool stdlib = true) {
    AnalysisResult r;
    auto file = File::from_source("test.sg", source);
    r.fileset.add_file(std::move(file));

    Parser parser(r.fileset);
    r.ast = parser.parse();

    r.analyzer = std::make_unique<Analyzer>(r.fileset);
    r.analyzer->is_stdlib = stdlib;
    if (r.ast && parser.errors.errors.empty()) {
      r.analyzer->analyze(*r.ast);
    } else {
      r.analyzer->errors.errors.insert(r.analyzer->errors.errors.end(),
                                       parser.errors.errors.begin(),
                                       parser.errors.errors.end());
    }
    return r;
  }

  size_t error_count() const { return analyzer->errors.errors.size(); }
  bool has_error() const { return error_count() > 0; }
  bool has_no_errors() const { return error_count() == 0; }

  bool has_error_containing(const std::string &substr) const {
    for (auto &err : analyzer->errors.errors) {
      if (err.message.find(substr) != std::string::npos)
        return true;
    }
    return false;
  }
};

// ===========================================================================
// Scope helpers
// ===========================================================================

TEST(Analyzer, PushPopScope) {
  FileSet fs;
  Analyzer a(fs);

  EXPECT_EQ(a.current_scope->kind, ScopeKind::Global);
  a.push_scope(ScopeKind::Function);
  EXPECT_EQ(a.current_scope->kind, ScopeKind::Function);
  a.push_scope(ScopeKind::Loop);
  EXPECT_EQ(a.current_scope->kind, ScopeKind::Loop);
  a.pop_scope();
  EXPECT_EQ(a.current_scope->kind, ScopeKind::Function);
  a.pop_scope();
  EXPECT_EQ(a.current_scope->kind, ScopeKind::Global);
}

TEST(Analyzer, DeclareAndLookup) {
  FileSet fs;
  Analyzer a(fs);

  auto sym = Symbol::variable("x", make_int_type(), Span{0, 0});
  EXPECT_TRUE(a.declare(sym));

  auto found = a.lookup("x");
  ASSERT_TRUE(found.has_value());
  EXPECT_EQ(found->name, "x");
}

TEST(Analyzer, DuplicateDeclarationReportsError) {
  FileSet fs;
  fs.add_file(File::from_source("test.sg", ""));
  Analyzer a(fs);

  auto sym = Symbol::variable("x", make_int_type(), Span{0, 0});
  EXPECT_TRUE(a.declare(sym));
  EXPECT_FALSE(a.declare(sym)); // duplicate
  EXPECT_EQ(a.errors.errors.size(), 1u);
}

TEST(Analyzer, BuiltinsAvailable) {
  FileSet fs;
  Analyzer a(fs);

  // Built-in types and constants are in the global scope.
  EXPECT_TRUE(a.lookup("int").has_value());
  EXPECT_TRUE(a.lookup("string").has_value());
  EXPECT_TRUE(a.lookup("true").has_value());
  EXPECT_TRUE(a.lookup("false").has_value());
}

// ===========================================================================
// Type-parameter helpers
// ===========================================================================

TEST(Analyzer, FreshTypeParamIds) {
  FileSet fs;
  Analyzer a(fs);

  auto id1 = a.fresh_type_param_id();
  auto id2 = a.fresh_type_param_id();
  EXPECT_NE(id1, id2);
}

// ===========================================================================
// Error reporting
// ===========================================================================

TEST(Analyzer, ErrorReporting) {
  FileSet fs;
  fs.add_file(File::from_source("test.sg", "hello"));
  Analyzer a(fs);

  a.error(Span{0, 5}, "test error");
  ASSERT_EQ(a.errors.errors.size(), 1u);
  EXPECT_EQ(a.errors.errors[0].message, "test error");
}

TEST(Analyzer, TypeErrorFormatting) {
  FileSet fs;
  fs.add_file(File::from_source("test.sg", "x"));
  Analyzer a(fs);

  a.type_error(Span{0, 1}, make_int_type(), make_string_type(), "assignment");
  ASSERT_EQ(a.errors.errors.size(), 1u);
  EXPECT_NE(a.errors.errors[0].message.find("int"), std::string::npos);
  EXPECT_NE(a.errors.errors[0].message.find("string"), std::string::npos);
}

// ===========================================================================
// declare_local — shadowing detection
// ===========================================================================

TEST(Analyzer, DeclareLocalSuccess) {
  FileSet fs;
  fs.add_file(File::from_source("test.sg", ""));
  Analyzer a(fs);

  a.push_scope(ScopeKind::Function);
  auto sym = Symbol::variable("x", make_int_type(), Span{0, 0});
  EXPECT_TRUE(a.declare_local(sym));
  EXPECT_TRUE(a.lookup("x").has_value());
  a.pop_scope();
}

TEST(Analyzer, DeclareLocalRedeclaration) {
  FileSet fs;
  fs.add_file(File::from_source("test.sg", ""));
  Analyzer a(fs);

  a.push_scope(ScopeKind::Function);
  auto sym = Symbol::variable("x", make_int_type(), Span{0, 0});
  EXPECT_TRUE(a.declare_local(sym));
  EXPECT_FALSE(a.declare_local(sym)); // same scope redeclaration
  EXPECT_TRUE(a.has_error_containing("already declared"));
  a.pop_scope();
}

TEST(Analyzer, DeclareLocalShadowingError) {
  FileSet fs;
  fs.add_file(File::from_source("test.sg", ""));
  Analyzer a(fs);

  a.push_scope(ScopeKind::Function);
  a.declare_local(Symbol::variable("x", make_int_type(), Span{0, 0}));

  a.push_scope(ScopeKind::Block);
  // This should fail: shadows outer scope's "x".
  EXPECT_FALSE(a.declare_local(
      Symbol::variable("x", make_string_type(), Span{5, 6})));
  EXPECT_TRUE(a.has_error_containing("shadows"));
  a.pop_scope();
  a.pop_scope();
}

// ===========================================================================
// Declaration collection (Phase 1)
// ===========================================================================

TEST(Analyzer, CollectFuncDecl) {
  auto r = AnalysisResult::from("fn foo() {}");
  EXPECT_TRUE(r.has_no_errors());
}

TEST(Analyzer, CollectStructDecl) {
  auto r = AnalysisResult::from("struct Point { x, y int }");
  EXPECT_TRUE(r.has_no_errors());
}

TEST(Analyzer, CollectEnumDecl) {
  auto r = AnalysisResult::from("enum Color { Red\nGreen\nBlue }");
  EXPECT_TRUE(r.has_no_errors());
}

TEST(Analyzer, CollectErrorDecl) {
  auto r = AnalysisResult::from("error NetworkError { code int }");
  EXPECT_TRUE(r.has_no_errors());
}

TEST(Analyzer, ResolveErrorInjectsMessageField) {
  auto r = AnalysisResult::from("error NetworkError { code int }");
  ASSERT_TRUE(r.has_no_errors());
  auto &syms = r.analyzer->package_scope_->symbols;
  auto it = syms.find("NetworkError");
  ASSERT_NE(it, syms.end());
  ASSERT_EQ(it->second.type->kind, TypeKind::Struct);
  auto &info = std::get<StructTypeInfo>(it->second.type->detail);
  EXPECT_TRUE(info.is_error);
  ASSERT_EQ(info.fields.size(), 3u);
  EXPECT_EQ(info.fields[0].name, "type_id"); // injected, field 0 (boxed id)
  EXPECT_EQ(info.fields[1].name, "message"); // injected, field 1
  ASSERT_NE(info.fields[1].type, nullptr);
  EXPECT_EQ(info.fields[1].type->kind, TypeKind::String);
  EXPECT_EQ(info.fields[2].name, "code");
}

TEST(Analyzer, ErrorMessageDefaultAccepted) {
  auto r = AnalysisResult::from(
      "error NetworkError {\n  message = \"a network error occurred\"\n}");
  EXPECT_TRUE(r.has_no_errors());
}

TEST(Analyzer, ErrorExplicitMessageFieldRejected) {
  // An explicit `message` field collides with the auto-injected one.
  auto r = AnalysisResult::from("error E {\n  message string\n}");
  EXPECT_TRUE(r.has_error_containing("duplicate field 'message'"));
}

TEST(Analyzer, ErrorNonComptimeMessageRejected) {
  auto r = AnalysisResult::from(
      "fn compute() string { \"x\" }\n"
      "error E {\n  message = compute()\n}");
  EXPECT_FALSE(r.has_no_errors());
}

TEST(Analyzer, ErrorReceiverMethodRejected) {
  auto r = AnalysisResult::from(
      "error E { code int }\n"
      "fn (e E) Tag() int { e.code }");
  EXPECT_TRUE(
      r.has_error_containing("methods cannot be attached to error type 'E'"));
}

TEST(Analyzer, ErrorTypeMethodRejected) {
  auto r = AnalysisResult::from(
      "error E { code int }\n"
      "fn E.New() int { 0 }");
  EXPECT_TRUE(
      r.has_error_containing("methods cannot be attached to error type 'E'"));
}

TEST(Analyzer, CollectMultipleDecls) {
  auto r = AnalysisResult::from(
      "fn foo() {}\nfn bar() {}\nstruct Baz {}");
  EXPECT_TRUE(r.has_no_errors());
}

// ===========================================================================
// Declaration type resolution (Phase 2b)
// ===========================================================================

TEST(Analyzer, ResolveFuncSignature) {
  auto r = AnalysisResult::from("fn Add(a, b int) int { a }");
  EXPECT_TRUE(r.has_no_errors());
}

TEST(Analyzer, ResolveStructFields) {
  auto r = AnalysisResult::from("struct Point { x, y int }");
  EXPECT_TRUE(r.has_no_errors());
}

TEST(Analyzer, ResolveEnumVariants) {
  auto r = AnalysisResult::from("enum Dir { Up\nDown\nLeft\nRight }");
  EXPECT_TRUE(r.has_no_errors());
}

TEST(Analyzer, ResolveInterfaceMethods) {
  auto r = AnalysisResult::from("interface Reader { Read() string }");
  EXPECT_TRUE(r.has_no_errors());
}

TEST(Analyzer, EmptyInterfaceReportsError) {
  auto r = AnalysisResult::from("interface Anything {}");
  EXPECT_TRUE(r.has_error_containing("must declare at least one method"));
}

TEST(Analyzer, InterfaceEmbedFlattensMethods) {
  auto r = AnalysisResult::from(
      "interface Reader { Read() string }\n"
      "interface Writer { Write(s string) void }\n"
      "interface ReadWriter {\n"
      "  Reader\n"
      "  Writer\n"
      "  Flush() void\n"
      "}\n");
  ASSERT_TRUE(r.has_no_errors());

  auto &syms = r.analyzer->package_scope_->symbols;
  auto it = syms.find("ReadWriter");
  ASSERT_NE(it, syms.end());
  ASSERT_EQ(it->second.type->kind, TypeKind::Interface);
  auto &info = std::get<InterfaceTypeInfo>(it->second.type->detail);
  ASSERT_EQ(info.methods.size(), 3u);
  std::set<std::string> names;
  for (auto &m : info.methods) names.insert(m.name);
  EXPECT_EQ(names, (std::set<std::string>{"Read", "Write", "Flush"}));
}

TEST(Analyzer, InterfaceEmbedIsTransitive) {
  auto r = AnalysisResult::from(
      "interface A { Foo() void }\n"
      "interface B { A\n  Bar() void }\n"
      "interface C { B\n  Baz() void }\n");
  ASSERT_TRUE(r.has_no_errors());

  auto &syms = r.analyzer->package_scope_->symbols;
  auto &info = std::get<InterfaceTypeInfo>(syms.find("C")->second.type->detail);
  EXPECT_EQ(info.methods.size(), 3u);
}

TEST(Analyzer, InterfaceEmbedOverlappingSameShapeMerges) {
  auto r = AnalysisResult::from(
      "interface Reader { Read() string }\n"
      "interface ReadWriter { Reader\n  Write(s string) void }\n"
      "interface ReadCloser { Reader\n  Close() void }\n"
      "interface Everything { ReadWriter\n  ReadCloser }\n");
  ASSERT_TRUE(r.has_no_errors());

  auto &syms = r.analyzer->package_scope_->symbols;
  auto &info =
      std::get<InterfaceTypeInfo>(syms.find("Everything")->second.type->detail);
  std::set<std::string> names;
  for (auto &m : info.methods) names.insert(m.name);
  EXPECT_EQ(names, (std::set<std::string>{"Read", "Write", "Close"}));
  EXPECT_EQ(info.methods.size(), 3u); // Read merged, not duplicated
}

TEST(Analyzer, InterfaceEmbedConflictReportsError) {
  auto r = AnalysisResult::from(
      "interface P { Do() void }\n"
      "interface Q { Do(x int) void }\n"
      "interface PQ { P\n  Q }\n");
  EXPECT_TRUE(r.has_error_containing("conflicts"));
}

TEST(Analyzer, InterfaceEmbedCycleReportsError) {
  auto r = AnalysisResult::from(
      "interface A { B\n  Foo() void }\n"
      "interface B { A\n  Bar() void }\n");
  EXPECT_TRUE(r.has_error_containing("cycle"));
}

TEST(Analyzer, InterfaceEmbedNonInterfaceReportsError) {
  auto r = AnalysisResult::from(
      "struct S { x int }\n"
      "interface I { S\n  Foo() void }\n");
  EXPECT_TRUE(r.has_error_containing("not an interface"));
}

TEST(Analyzer, ResolveConstWithType) {
  auto r = AnalysisResult::from("const Pi float = 3.14");
  EXPECT_TRUE(r.has_no_errors());
}

TEST(Analyzer, ResolveStructWithEmbed) {
  auto r = AnalysisResult::from(
      "struct Base { x int }\n"
      "struct Child {\n  Base\n  y int\n}");
  EXPECT_TRUE(r.has_no_errors());
}

TEST(Analyzer, ResolveStructWithUnknownEmbed) {
  auto r = AnalysisResult::from("struct Child {\n  Unknown\n  y int\n}");
  EXPECT_TRUE(r.has_error_containing("undefined"));
}

TEST(Analyzer, FieldDefaultComptimeAccepted) {
  auto r = AnalysisResult::from(
      "struct S {\n  x int = 1\n  y string = \"a\"\n}");
  EXPECT_TRUE(r.has_no_errors());
}

TEST(Analyzer, FieldDefaultNonComptimeRejected) {
  auto r = AnalysisResult::from(
      "fn f() int { 1 }\n"
      "struct S {\n  x int = f()\n}");
  EXPECT_TRUE(r.has_error_containing("compile-time constant"));
}

TEST(Analyzer, FieldDefaultTypeMismatchRejected) {
  auto r = AnalysisResult::from("struct S {\n  x int = \"no\"\n}");
  EXPECT_TRUE(r.has_error_containing("default for field 'x'"));
}

TEST(Analyzer, ResolveTransitiveEmbedField) {
  auto r = AnalysisResult::from(
      "struct A { x int }\n"
      "struct B {\n  A\n  y int\n}\n"
      "struct C {\n  B\n  z int\n}\n"
      "fn f(c C) int { c.x }");
  EXPECT_TRUE(r.has_no_errors());
}

TEST(Analyzer, ResolveTransitiveEmbedMethod) {
  auto r = AnalysisResult::from(
      "struct A { x int }\n"
      "fn (a A) Get() int { a.x }\n"
      "struct B {\n  A\n  y int\n}\n"
      "struct C {\n  B\n  z int\n}\n"
      "fn f(c C) int { c.Get() }");
  EXPECT_TRUE(r.has_no_errors());
}

TEST(Analyzer, ResolveUnknownTypeInSignature) {
  auto r = AnalysisResult::from("fn foo(x Nonexistent) {}");
  EXPECT_TRUE(r.has_error_containing("undefined"));
}

// ===========================================================================
// Name resolution in function bodies (Phase 3)
// ===========================================================================

TEST(Analyzer, ResolveIdentifierInBody) {
  auto r = AnalysisResult::from("fn foo(x int) int { x }");
  EXPECT_TRUE(r.has_no_errors());
}

TEST(Analyzer, ResolveUndefinedInBody) {
  auto r = AnalysisResult::from("fn foo() { y }");
  EXPECT_TRUE(r.has_error_containing("undefined name 'y'"));
}

TEST(Analyzer, ResolveVarDecl) {
  auto r = AnalysisResult::from(
      "fn foo() {\n"
      "  x int = 1\n"
      "  x\n"
      "}");
  EXPECT_TRUE(r.has_no_errors());
}

TEST(Analyzer, ResolveDeclAssign) {
  auto r = AnalysisResult::from(
      "fn foo() {\n"
      "  x := 1\n"
      "  x\n"
      "}");
  EXPECT_TRUE(r.has_no_errors());
}

TEST(Analyzer, ResolveVarDeclRedeclaresParam) {
  // Parameters and the body share the function scope, so redeclaring
  // a parameter name is a same-scope redeclaration, not a shadow.
  auto r = AnalysisResult::from(
      "fn foo(x int) {\n"
      "  x int = 2\n"
      "}");
  EXPECT_TRUE(r.has_error_containing("already declared"));
}

TEST(Analyzer, ResolveDeclAssignRedeclaresParam) {
  auto r = AnalysisResult::from(
      "fn foo(x int) {\n"
      "  x := 2\n"
      "}");
  EXPECT_TRUE(r.has_error_containing("already declared"));
}

TEST(Analyzer, ResolveNestedBlocks) {
  auto r = AnalysisResult::from(
      "fn foo() {\n"
      "  x := 1\n"
      "  if true {\n"
      "    x\n"
      "  }\n"
      "}");
  EXPECT_TRUE(r.has_no_errors());
}

TEST(Analyzer, ResolveShadowInNestedBlock) {
  auto r = AnalysisResult::from(
      "fn foo() {\n"
      "  x := 1\n"
      "  if true {\n"
      "    x := 2\n"
      "  }\n"
      "}");
  EXPECT_TRUE(r.has_error_containing("shadows"));
}

TEST(Analyzer, ResolveCallExpr) {
  auto r = AnalysisResult::from(
      "fn bar() int { 1 }\n"
      "fn foo() { bar() }");
  EXPECT_TRUE(r.has_no_errors());
}

TEST(Analyzer, ResolveBreakInsideLoop) {
  auto r = AnalysisResult::from(
      "fn foo() {\n"
      "  for { break }\n"
      "}");
  EXPECT_TRUE(r.has_no_errors());
}

TEST(Analyzer, ResolveReturnInsideFunction) {
  auto r = AnalysisResult::from(
      "fn foo() int {\n"
      "  return 42\n"
      "}");
  EXPECT_TRUE(r.has_no_errors());
}

TEST(Analyzer, ResolveForRangeVars) {
  auto r = AnalysisResult::from(
      "fn foo() {\n"
      "  arr := [1, 2, 3]\n"
      "  for v : arr {\n"
      "    v\n"
      "  }\n"
      "}");
  EXPECT_TRUE(r.has_no_errors());
}

TEST(Analyzer, ResolveForIterClause) {
  auto r = AnalysisResult::from(
      "fn foo() {\n"
      "  for i := 0; i < 10; i += 1 {\n"
      "    i\n"
      "  }\n"
      "}");
  EXPECT_TRUE(r.has_no_errors());
}

TEST(Analyzer, ResolveOrExprPipe) {
  // The |err| pipe variable should be visible inside the or block.
  auto r = AnalysisResult::from(
      "fn foo() {\n"
      "  x := 1\n"
      "  x or |err| { err }\n"
      "}");
  EXPECT_TRUE(r.has_no_errors());
}

TEST(Analyzer, ResolveFuncExpr) {
  auto r = AnalysisResult::from(
      "fn foo() {\n"
      "  f := fn(x int) int { x }\n"
      "  f\n"
      "}");
  EXPECT_TRUE(r.has_no_errors());
}

TEST(Analyzer, ResolveStringInterpolation) {
  auto r = AnalysisResult::from(
      "fn foo(name string) string {\n"
      "  \"hello {name}\"\n"
      "}");
  EXPECT_TRUE(r.has_no_errors());
}

TEST(Analyzer, ResolveStructLiteral) {
  auto r = AnalysisResult::from(
      "struct Point { x, y int }\n"
      "fn foo() { Point{x: 1, y: 2} }");
  EXPECT_TRUE(r.has_no_errors());
}

TEST(Analyzer, ResolveMapLiteral) {
  auto r = AnalysisResult::from(
      "fn foo() { {\"a\": 1, \"b\": 2} }");
  EXPECT_TRUE(r.has_no_errors());
}

TEST(Analyzer, ResolveIfElse) {
  auto r = AnalysisResult::from(
      "fn foo(x int) int {\n"
      "  if x > 0 { x } else { 0 }\n"
      "}");
  EXPECT_TRUE(r.has_no_errors());
}

TEST(Analyzer, ResolveSwitchExpr) {
  auto r = AnalysisResult::from(
      "fn foo(x int) int {\n"
      "  switch x {\n"
      "    case 0: 0\n"
      "    case 1: 1\n"
      "    else: x\n"
      "  }\n"
      "}");
  EXPECT_TRUE(r.has_no_errors());
}

TEST(Analyzer, ResolveSpawnExpr) {
  auto r = AnalysisResult::from(
      "fn foo() {\n"
      "  spawn |task| { task }\n"
      "}");
  EXPECT_TRUE(r.has_no_errors());
}

TEST(Analyzer, ResolveReceiverMethod) {
  auto r = AnalysisResult::from(
      "struct Point { x, y int }\n"
      "fn (p Point) Sum() int { p.x }");
  EXPECT_TRUE(r.has_no_errors());
}

TEST(Analyzer, ResolveMultipleParams) {
  auto r = AnalysisResult::from(
      "fn foo(a, b int, c string) { a\nb\nc }");
  EXPECT_TRUE(r.has_no_errors());
}

TEST(Analyzer, ResolveIgnoredIdentifier) {
  // Ignored identifiers (starting with _) should not trigger undefined errors.
  auto r = AnalysisResult::from(
      "fn foo() {\n"
      "  _ := 1\n"
      "}");
  EXPECT_TRUE(r.has_no_errors());
}

TEST(Analyzer, ResolveAccumulator) {
  auto r = AnalysisResult::from(
      "fn foo() {\n"
      "  arr := [1, 2, 3]\n"
      "  for i : arr |acc| {\n"
      "    acc\n"
      "  }\n"
      "}");
  EXPECT_TRUE(r.has_no_errors());
}

TEST(Analyzer, DuplicateTopLevelDecl) {
  auto r = AnalysisResult::from(
      "fn foo() {}\n"
      "fn foo() {}");
  EXPECT_TRUE(r.has_error_containing("already declared"));
}

TEST(Analyzer, UseBeforeDeclare) {
  auto r = AnalysisResult::from(
      "fn foo() {\n"
      "  y\n"
      "  y := 1\n"
      "}");
  // "y" used on line 2 but declared on line 3 — should be undefined.
  EXPECT_TRUE(r.has_error_containing("undefined name 'y'"));
}

TEST(Analyzer, ForwardReferenceTopLevel) {
  // Top-level functions can reference each other (collected in pass 1).
  auto r = AnalysisResult::from(
      "fn foo() { bar() }\n"
      "fn bar() { foo() }");
  EXPECT_TRUE(r.has_no_errors());
}

TEST(Analyzer, BuiltinTypeInSignature) {
  auto r = AnalysisResult::from(
      "fn foo(x int, y float, z string, w bool) void {}");
  EXPECT_TRUE(r.has_no_errors());
}

TEST(Analyzer, SizedTypesInSignature) {
  auto r = AnalysisResult::from(
      "fn foo(a int8, b uint32, c float64, d byte) {}");
  EXPECT_TRUE(r.has_no_errors());
}

TEST(Analyzer, InternalTypesInSignature) {
  auto r = AnalysisResult::from(
      "fn foo(e error, m Missing, c Comparison) {}");
  EXPECT_TRUE(r.has_no_errors());
}

// ===========================================================================
// Intrinsic type receiver methods
// ===========================================================================

TEST(Analyzer, IntrinsicReceiverMethodStdlibAllowed) {
  AnalysisResult r;
  auto file = File::from_source("test.sg",
      "fn (self string) TestMethod() int { 42 }\n"
      "pub fn Main() void {}");
  r.fileset.add_file(std::move(file));
  Parser parser(r.fileset);
  r.ast = parser.parse();
  r.analyzer = std::make_unique<Analyzer>(r.fileset);
  r.analyzer->is_stdlib = true;
  r.analyzer->analyze(*r.ast);
  EXPECT_TRUE(r.has_no_errors());
}

TEST(Analyzer, IntrinsicReceiverMethodNonStdlibRejected) {
  AnalysisResult r;
  auto file = File::from_source("test.sg",
      "fn (self string) TestMethod() int { 42 }\n"
      "pub fn Main() void {}");
  r.fileset.add_file(std::move(file));
  Parser parser(r.fileset);
  r.ast = parser.parse();
  r.analyzer = std::make_unique<Analyzer>(r.fileset);
  r.analyzer->is_stdlib = false;
  r.analyzer->analyze(*r.ast);
  EXPECT_TRUE(r.has_error_containing("intrinsic types"));
}

TEST(Analyzer, IntrinsicReceiverMethodResolvesInCheckSelector) {
  AnalysisResult r;
  auto file = File::from_source("test.sg",
      "fn (self int) Double() int { self * 2 }\n"
      "pub fn Main() void {\n"
      "  x := 5\n"
      "  y := x.Double()\n"
      "}");
  r.fileset.add_file(std::move(file));
  Parser parser(r.fileset);
  r.ast = parser.parse();
  r.analyzer = std::make_unique<Analyzer>(r.fileset);
  r.analyzer->is_stdlib = true;
  r.analyzer->analyze(*r.ast);
  EXPECT_TRUE(r.has_no_errors());
}

TEST(Analyzer, ErrorInSecondFileReportsThatFile) {
  AnalysisResult r;
  r.fileset.add_file(File::from_source("a.sg", "pub fn Main() void {}\n"));
  r.fileset.add_file(File::from_source("b.sg", "fn helper() int {\n"
                                               "  return notAThing\n"
                                               "}\n"));
  Parser parser(r.fileset);
  r.ast = parser.parse();
  r.analyzer = std::make_unique<Analyzer>(r.fileset);
  r.analyzer->is_stdlib = false;
  r.analyzer->analyze(*r.ast);

  ASSERT_TRUE(r.has_error_containing("notAThing"));
  auto &err = r.analyzer->errors.errors.front();
  EXPECT_EQ(err.p.filename, "b.sg");
  EXPECT_EQ(err.p.line, 2u);
}

} // namespace saga

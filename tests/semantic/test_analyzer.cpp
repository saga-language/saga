// Copywrite 2026 Rob Thornton
// SPDX-License-Identifier: MIT

#include "semantic/analyzer.hpp"
#include "frontend/file.hpp"
#include "frontend/fileset.hpp"
#include "frontend/parser.hpp"

#include <gtest/gtest.h>

namespace mc {

// ---------------------------------------------------------------------------
// Helper — parse source text and run the analyzer, returning both.
// ---------------------------------------------------------------------------

struct AnalysisResult {
  FileSet fileset;
  NodePtr ast;
  std::unique_ptr<Analyzer> analyzer;

  static AnalysisResult from(const std::string &source) {
    AnalysisResult r;
    auto file = File::from_source("test.mc", source);
    r.fileset.add_file(std::move(file));

    Parser parser(r.fileset);
    r.ast = parser.parse();

    r.analyzer = std::make_unique<Analyzer>(r.fileset);
    if (r.ast) {
      r.analyzer->analyze(*r.ast);
    }
    return r;
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
  fs.add_file(File::from_source("test.mc", ""));
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
  EXPECT_TRUE(a.lookup("Int").has_value());
  EXPECT_TRUE(a.lookup("String").has_value());
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
  fs.add_file(File::from_source("test.mc", "hello"));
  Analyzer a(fs);

  a.error(Span{0, 5}, "test error");
  ASSERT_EQ(a.errors.errors.size(), 1u);
  EXPECT_EQ(a.errors.errors[0].message, "test error");
}

TEST(Analyzer, TypeErrorFormatting) {
  FileSet fs;
  fs.add_file(File::from_source("test.mc", "x"));
  Analyzer a(fs);

  a.type_error(Span{0, 1}, make_int_type(), make_string_type(), "assignment");
  ASSERT_EQ(a.errors.errors.size(), 1u);
  EXPECT_NE(a.errors.errors[0].message.find("Int"), std::string::npos);
  EXPECT_NE(a.errors.errors[0].message.find("String"), std::string::npos);
}

// ===========================================================================
// Declaration collection (Phase 1)
// ===========================================================================

TEST(Analyzer, CollectFuncDecl) {
  auto r = AnalysisResult::from("fn foo() {}");
  // The function name should be collected in the module scope.
  // We check indirectly: no errors should be reported.
  EXPECT_EQ(r.analyzer->errors.errors.size(), 0u);
}

TEST(Analyzer, CollectStructDecl) {
  auto r = AnalysisResult::from("struct Point { x Int\ny Int }");
  EXPECT_EQ(r.analyzer->errors.errors.size(), 0u);
}

TEST(Analyzer, CollectEnumDecl) {
  auto r = AnalysisResult::from("enum Color { Red\nGreen\nBlue }");
  EXPECT_EQ(r.analyzer->errors.errors.size(), 0u);
}

TEST(Analyzer, CollectMultipleDecls) {
  auto r = AnalysisResult::from(
      "fn foo() {}\nfn bar() {}\nstruct Baz {}");
  EXPECT_EQ(r.analyzer->errors.errors.size(), 0u);
}

} // namespace mc

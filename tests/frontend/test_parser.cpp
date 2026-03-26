#include "frontend/ast.hpp"
#include "frontend/file.hpp"
#include "frontend/fileset.hpp"
#include "frontend/parser.hpp"

#include <gtest/gtest.h>

// =============================================================================
// Parser helper tests
//
// The parser's token/span/Pratt helpers are all private, so they are exercised
// here through the only public surface that exists today: constructing a Parser
// and confirming that infrastructure-level behaviour is correct.
//
// Full AST-level tests (parse_expression, parse_declaration, etc.) will be
// added in later implementation phases. Each section below is labelled with
// the phase it corresponds to in docs/parser-plan.md.
// =============================================================================

namespace mc {

// ---------------------------------------------------------------------------
// ParseResult — owns the FileSet (and therefore all string_view lifetimes),
// the AST root, and the error list.  Use ParseResult::from() in every test
// that needs to inspect the AST or error list after parsing.
// ---------------------------------------------------------------------------

struct ParseResult {
  FileSet       fs;
  NodePtr       root;
  std::vector<Error> errors;

  static ParseResult from(const std::string &source) {
    ParseResult r;
    r.fs.add_file(File::from_source("test.rg", source));
    Parser p(r.fs);
    r.root   = p.parse();
    r.errors = p.errors.errors;
    return r;
  }

  // Unwrap the single SourceNode produced for "test.rg".
  const SourceNode &source_node() const {
    return std::get<SourceNode>(
        std::get<PackageNode>(root->data).sources.at(0)->data);
  }

  // Unwrap the Nth top-level declaration as type T.
  // Returns nullptr if the index is out of range or the type does not match,
  // so callers can use ASSERT_NE(r.decl_as<T>(n), nullptr) before accessing.
  template <typename T>
  const T *decl_as(size_t index) const {
    const auto &decls = source_node().declarations;
    if (index >= decls.size()) return nullptr;
    return std::get_if<T>(&decls.at(index)->data);
  }
};

// ---------------------------------------------------------------------------
// Phase 0 — Construction & error infrastructure
// ---------------------------------------------------------------------------

TEST(Parser, Construction_NoErrors) {
  auto fs = FileSet{};
  fs.add_file(File::from_source("test.rg", ""));
  Parser p(fs);

  EXPECT_EQ(p.errors.errors.size(), 0u);
  EXPECT_FALSE(p.errors.max_reached());
}

// Parsing an empty file must produce a PackageNode containing one empty
// SourceNode — not a crash, not nullptr.
TEST(Parser, Parse_EmptySource_ReturnsPackageNode) {
  auto fs = FileSet{};
  fs.add_file(File::from_source("test.rg", ""));
  Parser p(fs);

  auto result = p.parse();
  ASSERT_NE(result, nullptr);

  auto *pkg = std::get_if<PackageNode>(&result->data);
  ASSERT_NE(pkg, nullptr);
  ASSERT_EQ(pkg->sources.size(), 1u);

  auto *src = std::get_if<SourceNode>(&pkg->sources[0]->data);
  ASSERT_NE(src, nullptr);
  EXPECT_TRUE(src->declarations.empty());
}

// ---------------------------------------------------------------------------
// Phase 2 — Pratt binding power tables
//
// infix_binding_power and prefix_binding_power are public static methods —
// pure lookup functions with no parser state dependency — so they can be
// called and tested directly.
// ---------------------------------------------------------------------------

class ParserPrattTest : public ::testing::Test {};

// ── Non-operators return 0 ────────────────────────────────────────────────

TEST_F(ParserPrattTest, InfixBP_NonOperator_ReturnsZero) {
  EXPECT_EQ(Parser::infix_binding_power(Token::Kind::Identifier), 0);
  EXPECT_EQ(Parser::infix_binding_power(Token::Kind::IntegerLiteral), 0);
  EXPECT_EQ(Parser::infix_binding_power(Token::Kind::RightBrace), 0);
  EXPECT_EQ(Parser::infix_binding_power(Token::Kind::Terminator), 0);
  EXPECT_EQ(Parser::infix_binding_power(Token::Kind::Eof), 0);
}

TEST_F(ParserPrattTest, PrefixBP_NonOperator_ReturnsZero) {
  EXPECT_EQ(Parser::prefix_binding_power(Token::Kind::Identifier), 0);
  EXPECT_EQ(Parser::prefix_binding_power(Token::Kind::IntegerLiteral), 0);
  EXPECT_EQ(Parser::prefix_binding_power(Token::Kind::Add), 0);
  EXPECT_EQ(Parser::prefix_binding_power(Token::Kind::Multiply), 0);
}

// ── Access operators are the highest infix precedence ────────────────────

TEST_F(ParserPrattTest, InfixBP_AccessOperators_Highest) {
  int dot = Parser::infix_binding_power(Token::Kind::Dot);
  int lbrak = Parser::infix_binding_power(Token::Kind::LeftBracket);
  int lparen = Parser::infix_binding_power(Token::Kind::LeftParenthesis);
  int pow = Parser::infix_binding_power(Token::Kind::Pow);

  EXPECT_EQ(dot, lbrak);
  EXPECT_EQ(dot, lparen);
  EXPECT_GT(dot, pow); // access > exponentiation
}

// ── Precedence ordering across all levels ────────────────────────────────

TEST_F(ParserPrattTest, InfixBP_PrecedenceOrder) {
  int access = Parser::infix_binding_power(Token::Kind::Dot);
  int pow = Parser::infix_binding_power(Token::Kind::Pow);
  int mul = Parser::infix_binding_power(Token::Kind::Multiply);
  int add = Parser::infix_binding_power(Token::Kind::Add);
  int bitwise = Parser::infix_binding_power(Token::Kind::BitwiseAnd);
  int cmp = Parser::infix_binding_power(Token::Kind::Equal);
  int land = Parser::infix_binding_power(Token::Kind::LogicalAnd);
  int lor = Parser::infix_binding_power(Token::Kind::LogicalOr);
  int range = Parser::infix_binding_power(Token::Kind::DotDot);
  int or_bp = Parser::infix_binding_power(Token::Kind::Or);

  EXPECT_GT(access, pow);
  EXPECT_GT(pow, mul);
  EXPECT_GT(mul, add);
  EXPECT_GT(add, bitwise);
  EXPECT_GT(bitwise, cmp);
  EXPECT_GT(cmp, land);
  EXPECT_GT(land, lor);
  EXPECT_GT(lor, range);
  EXPECT_GT(range, or_bp);
  EXPECT_GT(or_bp, 0);
}

// ── Operators within the same level share the same binding power ──────────

TEST_F(ParserPrattTest, InfixBP_SameLevelOperators_EqualBP) {
  // Multiplicative
  EXPECT_EQ(Parser::infix_binding_power(Token::Kind::Multiply),
            Parser::infix_binding_power(Token::Kind::Divide));
  EXPECT_EQ(Parser::infix_binding_power(Token::Kind::Multiply),
            Parser::infix_binding_power(Token::Kind::Modulo));

  // Additive
  EXPECT_EQ(Parser::infix_binding_power(Token::Kind::Add),
            Parser::infix_binding_power(Token::Kind::Sub));

  // Bitwise
  EXPECT_EQ(Parser::infix_binding_power(Token::Kind::BitwiseAnd),
            Parser::infix_binding_power(Token::Kind::BitwiseOr));
  EXPECT_EQ(Parser::infix_binding_power(Token::Kind::BitwiseAnd),
            Parser::infix_binding_power(Token::Kind::BitwiseXor));
  EXPECT_EQ(Parser::infix_binding_power(Token::Kind::BitwiseAnd),
            Parser::infix_binding_power(Token::Kind::LeftShift));
  EXPECT_EQ(Parser::infix_binding_power(Token::Kind::BitwiseAnd),
            Parser::infix_binding_power(Token::Kind::RightShift));

  // Comparison
  EXPECT_EQ(Parser::infix_binding_power(Token::Kind::Equal),
            Parser::infix_binding_power(Token::Kind::NotEqual));
  EXPECT_EQ(Parser::infix_binding_power(Token::Kind::Equal),
            Parser::infix_binding_power(Token::Kind::LessThan));
  EXPECT_EQ(Parser::infix_binding_power(Token::Kind::Equal),
            Parser::infix_binding_power(Token::Kind::LessThanEqual));
  EXPECT_EQ(Parser::infix_binding_power(Token::Kind::Equal),
            Parser::infix_binding_power(Token::Kind::GreaterThan));
  EXPECT_EQ(Parser::infix_binding_power(Token::Kind::Equal),
            Parser::infix_binding_power(Token::Kind::GreaterThanEqual));
}

// ── Prefix operators ─────────────────────────────────────────────────────

TEST_F(ParserPrattTest, PrefixBP_UnaryOperators_NonZero) {
  EXPECT_GT(Parser::prefix_binding_power(Token::Kind::Not), 0);
  EXPECT_GT(Parser::prefix_binding_power(Token::Kind::Sub), 0);
  EXPECT_GT(Parser::prefix_binding_power(Token::Kind::BitwiseNot), 0);
}

TEST_F(ParserPrattTest, PrefixBP_UnaryOperators_EqualBP) {
  EXPECT_EQ(Parser::prefix_binding_power(Token::Kind::Not),
            Parser::prefix_binding_power(Token::Kind::Sub));
  EXPECT_EQ(Parser::prefix_binding_power(Token::Kind::Not),
            Parser::prefix_binding_power(Token::Kind::BitwiseNot));
}

// Unary prefix must bind more tightly than ** so that `-2 ** 3 == (-2) ** 3`.
// (Grammar section 2 places Unary above Power.)
TEST_F(ParserPrattTest, PrefixBP_HigherThanPow) {
  EXPECT_GT(Parser::prefix_binding_power(Token::Kind::Sub),
            Parser::infix_binding_power(Token::Kind::Pow));
}

// Prefix binding must be lower than access operators so that `-arr[0]`
// parses as `-(arr[0])`, not `(-arr)[0]`.
TEST_F(ParserPrattTest, PrefixBP_LowerThanAccess) {
  EXPECT_LT(Parser::prefix_binding_power(Token::Kind::Sub),
            Parser::infix_binding_power(Token::Kind::LeftBracket));
}

// =============================================================================
// Phase 8 — parse_declaration
// =============================================================================

// ---------------------------------------------------------------------------
// ImportDecl — fully implemented
// ---------------------------------------------------------------------------

// A bare import produces one ImportDeclNode with the quotes stripped from
// the path.
TEST(ParserDeclaration, Import_Simple) {
  auto r = ParseResult::from("import \"std/io\"\n");

  EXPECT_TRUE(r.errors.empty());
  EXPECT_EQ(r.source_node().declarations.size(), 1u);

  auto *n = r.decl_as<ImportDeclNode>(0);
  ASSERT_NE(n, nullptr);
  EXPECT_EQ(n->path, "std/io");
}

// Deeper path — verify the strip works for paths with slashes.
TEST(ParserDeclaration, Import_NestedPath) {
  auto r = ParseResult::from("import \"mega/long/mathematics\"\n");

  EXPECT_TRUE(r.errors.empty());
  auto *n = r.decl_as<ImportDeclNode>(0);
  ASSERT_NE(n, nullptr);
  EXPECT_EQ(n->path, "mega/long/mathematics");
}

// Multiple imports on consecutive lines produce a declaration per import.
TEST(ParserDeclaration, Import_Multiple) {
  auto r = ParseResult::from("import \"std/io\"\nimport \"std/math\"\n");

  EXPECT_TRUE(r.errors.empty());
  ASSERT_EQ(r.source_node().declarations.size(), 2u);

  auto *n0 = r.decl_as<ImportDeclNode>(0);
  auto *n1 = r.decl_as<ImportDeclNode>(1);
  ASSERT_NE(n0, nullptr);
  ASSERT_NE(n1, nullptr);
  EXPECT_EQ(n0->path, "std/io");
  EXPECT_EQ(n1->path, "std/math");
}

// "pub import" is a semantic error but the parser still produces the node
// so downstream passes can continue running.
TEST(ParserDeclaration, Import_PubReportsErrorButProducesNode) {
  auto r = ParseResult::from("pub import \"std/io\"\n");

  EXPECT_EQ(r.errors.size(), 1u);
  // The ImportDeclNode is still present despite the error.
  ASSERT_EQ(r.source_node().declarations.size(), 1u);
  auto *n = r.decl_as<ImportDeclNode>(0);
  ASSERT_NE(n, nullptr);
  EXPECT_EQ(n->path, "std/io");
}

// ---------------------------------------------------------------------------
// parse_declaration dispatch — stub branches
//
// These tests verify that each keyword routes to its sub-parser without
// crashing, and that no spurious errors are reported for the dispatch itself.
// The sub-parsers are stubs; they consume the keyword, synchronize, and return
// nullptr.  A trailing newline gives synchronize() a clean termination point.
// ---------------------------------------------------------------------------

TEST(ParserDeclaration, Dispatch_Fn) {
  auto r = ParseResult::from("fn\n");
  EXPECT_TRUE(r.errors.empty());
  EXPECT_TRUE(r.source_node().declarations.empty());
}

TEST(ParserDeclaration, Dispatch_PubFn) {
  auto r = ParseResult::from("pub fn\n");
  EXPECT_TRUE(r.errors.empty());
  EXPECT_TRUE(r.source_node().declarations.empty());
}

TEST(ParserDeclaration, Dispatch_Const) {
  auto r = ParseResult::from("const\n");
  EXPECT_TRUE(r.errors.empty());
  EXPECT_TRUE(r.source_node().declarations.empty());
}

TEST(ParserDeclaration, Dispatch_Struct) {
  auto r = ParseResult::from("struct\n");
  EXPECT_TRUE(r.errors.empty());
  EXPECT_TRUE(r.source_node().declarations.empty());
}

TEST(ParserDeclaration, Dispatch_Enum) {
  auto r = ParseResult::from("enum\n");
  EXPECT_TRUE(r.errors.empty());
  EXPECT_TRUE(r.source_node().declarations.empty());
}

TEST(ParserDeclaration, Dispatch_Interface) {
  auto r = ParseResult::from("interface\n");
  EXPECT_TRUE(r.errors.empty());
  EXPECT_TRUE(r.source_node().declarations.empty());
}

// ---------------------------------------------------------------------------
// Error recovery
// ---------------------------------------------------------------------------

// A completely unknown token reports an error and does not crash.
TEST(ParserDeclaration, UnknownToken_ReportsError) {
  auto r = ParseResult::from("?\n");
  EXPECT_FALSE(r.errors.empty());
  EXPECT_TRUE(r.source_node().declarations.empty());
}

// "pub" with no following declaration keyword is an error.
TEST(ParserDeclaration, Pub_Alone_ReportsError) {
  auto r = ParseResult::from("pub\n");
  EXPECT_FALSE(r.errors.empty());
  EXPECT_TRUE(r.source_node().declarations.empty());
}

// A bad token between two valid imports: the bad token produces an error,
// but the parser recovers and the second import is still parsed.
TEST(ParserDeclaration, Recovery_BadTokenBetweenImports) {
  auto r = ParseResult::from(
      "import \"std/io\"\n"
      "?\n"
      "import \"std/math\"\n");

  EXPECT_EQ(r.errors.size(), 1u);
  ASSERT_EQ(r.source_node().declarations.size(), 2u);

  auto *n0 = r.decl_as<ImportDeclNode>(0);
  auto *n1 = r.decl_as<ImportDeclNode>(1);
  ASSERT_NE(n0, nullptr);
  ASSERT_NE(n1, nullptr);
  EXPECT_EQ(n0->path, "std/io");
  EXPECT_EQ(n1->path, "std/math");
}

// =============================================================================
// ExprResult — thin helper for expression-level unit tests.
//
// Initialises a single-file Parser, primes it via init_for_file(), then
// calls parse_expression() so tests can inspect the resulting AST node and
// error list without going through parse_source() / parse_declaration().
// =============================================================================

struct ExprResult {
  FileSet            fs;
  NodePtr            expr;
  std::vector<Error> errors;

  static ExprResult from(const std::string &source) {
    ExprResult r;
    r.fs.add_file(File::from_source("test.rg", source));
    Parser p(r.fs);
    p.init_for_file(r.fs.files[0].get());
    r.expr   = p.parse_expression();
    r.errors = p.errors.errors;
    return r;
  }

  // Unwrap the root expression as T; returns nullptr on type mismatch or when
  // expr is null.
  template <typename T>
  const T *as() const {
    if (!expr) return nullptr;
    return std::get_if<T>(&expr->data);
  }
};

// =============================================================================
// Phase — Group 1: parse_prefix (atoms, unary, group/range, array, import)
// =============================================================================

class ParserPrefixTest : public ::testing::Test {};

// ── Atoms ────────────────────────────────────────────────────────────────────

TEST_F(ParserPrefixTest, Atom_Integer) {
  auto r = ExprResult::from("42");
  EXPECT_TRUE(r.errors.empty());
  auto *n = r.as<IntegerLiteralNode>();
  ASSERT_NE(n, nullptr);
  EXPECT_EQ(n->literal, "42");
}

TEST_F(ParserPrefixTest, Atom_Float) {
  auto r = ExprResult::from("3.14");
  EXPECT_TRUE(r.errors.empty());
  auto *n = r.as<FloatLiteralNode>();
  ASSERT_NE(n, nullptr);
  EXPECT_EQ(n->literal, "3.14");
}

TEST_F(ParserPrefixTest, Atom_BoolTrue) {
  auto r = ExprResult::from("true");
  EXPECT_TRUE(r.errors.empty());
  auto *n = r.as<BoolLiteralNode>();
  ASSERT_NE(n, nullptr);
  EXPECT_EQ(n->literal, "true");
}

TEST_F(ParserPrefixTest, Atom_BoolFalse) {
  auto r = ExprResult::from("false");
  EXPECT_TRUE(r.errors.empty());
  auto *n = r.as<BoolLiteralNode>();
  ASSERT_NE(n, nullptr);
  EXPECT_EQ(n->literal, "false");
}

TEST_F(ParserPrefixTest, Atom_Identifier) {
  auto r = ExprResult::from("myVar");
  EXPECT_TRUE(r.errors.empty());
  auto *n = r.as<IdentifierNode>();
  ASSERT_NE(n, nullptr);
  EXPECT_EQ(n->name, "myVar");
}

TEST_F(ParserPrefixTest, Atom_Identifier_WithQuestionMark) {
  auto r = ExprResult::from("value?");
  EXPECT_TRUE(r.errors.empty());
  auto *n = r.as<IdentifierNode>();
  ASSERT_NE(n, nullptr);
  EXPECT_EQ(n->name, "value?");
}

TEST_F(ParserPrefixTest, Atom_String_Plain) {
  auto r = ExprResult::from("\"hello\"");
  EXPECT_TRUE(r.errors.empty());
  auto *n = r.as<StringLiteralNode>();
  ASSERT_NE(n, nullptr);
  ASSERT_EQ(n->fragments.size(), 1u);
  auto *frag = std::get_if<StringFragmentNode>(&n->fragments[0]->data);
  ASSERT_NE(frag, nullptr);
  EXPECT_EQ(frag->text, "\"hello\"");
}

// ── Unary operators ──────────────────────────────────────────────────────────

TEST_F(ParserPrefixTest, Unary_Not_Bool) {
  auto r = ExprResult::from("!true");
  EXPECT_TRUE(r.errors.empty());
  auto *n = r.as<UnaryExprNode>();
  ASSERT_NE(n, nullptr);
  EXPECT_EQ(n->op, Token::Kind::Not);
  auto *inner = std::get_if<BoolLiteralNode>(&n->operand->data);
  ASSERT_NE(inner, nullptr);
  EXPECT_EQ(inner->literal, "true");
}

TEST_F(ParserPrefixTest, Unary_Negate_Integer) {
  auto r = ExprResult::from("-42");
  EXPECT_TRUE(r.errors.empty());
  auto *n = r.as<UnaryExprNode>();
  ASSERT_NE(n, nullptr);
  EXPECT_EQ(n->op, Token::Kind::Sub);
  auto *inner = std::get_if<IntegerLiteralNode>(&n->operand->data);
  ASSERT_NE(inner, nullptr);
  EXPECT_EQ(inner->literal, "42");
}

TEST_F(ParserPrefixTest, Unary_BitwiseNot_Identifier) {
  auto r = ExprResult::from("~flags");
  EXPECT_TRUE(r.errors.empty());
  auto *n = r.as<UnaryExprNode>();
  ASSERT_NE(n, nullptr);
  EXPECT_EQ(n->op, Token::Kind::BitwiseNot);
  auto *inner = std::get_if<IdentifierNode>(&n->operand->data);
  ASSERT_NE(inner, nullptr);
  EXPECT_EQ(inner->name, "flags");
}

// Unary operators are right-associative with each other: `- -42` must parse
// as `-(-(42))`, not as something flat.
TEST_F(ParserPrefixTest, Unary_Double_Negate) {
  auto r = ExprResult::from("- -42");
  EXPECT_TRUE(r.errors.empty());
  auto *outer = r.as<UnaryExprNode>();
  ASSERT_NE(outer, nullptr);
  EXPECT_EQ(outer->op, Token::Kind::Sub);
  auto *inner = std::get_if<UnaryExprNode>(&outer->operand->data);
  ASSERT_NE(inner, nullptr);
  EXPECT_EQ(inner->op, Token::Kind::Sub);
  auto *lit = std::get_if<IntegerLiteralNode>(&inner->operand->data);
  ASSERT_NE(lit, nullptr);
  EXPECT_EQ(lit->literal, "42");
}

TEST_F(ParserPrefixTest, Unary_Not_Identifier) {
  auto r = ExprResult::from("!done");
  EXPECT_TRUE(r.errors.empty());
  auto *n = r.as<UnaryExprNode>();
  ASSERT_NE(n, nullptr);
  EXPECT_EQ(n->op, Token::Kind::Not);
}

// ── Grouped expressions ───────────────────────────────────────────────────────

TEST_F(ParserPrefixTest, Group_Integer) {
  auto r = ExprResult::from("(42)");
  EXPECT_TRUE(r.errors.empty());
  auto *n = r.as<GroupExprNode>();
  ASSERT_NE(n, nullptr);
  auto *inner = std::get_if<IntegerLiteralNode>(&n->inner->data);
  ASSERT_NE(inner, nullptr);
  EXPECT_EQ(inner->literal, "42");
}

TEST_F(ParserPrefixTest, Group_Identifier) {
  auto r = ExprResult::from("(x)");
  EXPECT_TRUE(r.errors.empty());
  auto *n = r.as<GroupExprNode>();
  ASSERT_NE(n, nullptr);
  auto *inner = std::get_if<IdentifierNode>(&n->inner->data);
  ASSERT_NE(inner, nullptr);
  EXPECT_EQ(inner->name, "x");
}

TEST_F(ParserPrefixTest, Group_Unary_Inside) {
  auto r = ExprResult::from("(!true)");
  EXPECT_TRUE(r.errors.empty());
  auto *n = r.as<GroupExprNode>();
  ASSERT_NE(n, nullptr);
  auto *inner = std::get_if<UnaryExprNode>(&n->inner->data);
  ASSERT_NE(inner, nullptr);
  EXPECT_EQ(inner->op, Token::Kind::Not);
}

// ── Range expressions ─────────────────────────────────────────────────────────

TEST_F(ParserPrefixTest, Range_IntegerLiterals) {
  // Spaces around ".." are required: the lexer greedily reads "1." as the
  // start of a float literal, so "1..10" tokenises as FloatLiteral("1.") +
  // Dot + IntegerLiteral("10") rather than the expected
  // IntegerLiteral("1") + DotDot + IntegerLiteral("10").
  // Tracked as a lexer bug to fix separately.
  auto r = ExprResult::from("(1 .. 10)");
  EXPECT_TRUE(r.errors.empty());
  auto *n = r.as<RangeExprNode>();
  ASSERT_NE(n, nullptr);
  auto *lo = std::get_if<IntegerLiteralNode>(&n->low->data);
  auto *hi = std::get_if<IntegerLiteralNode>(&n->high->data);
  ASSERT_NE(lo, nullptr);
  ASSERT_NE(hi, nullptr);
  EXPECT_EQ(lo->literal, "1");
  EXPECT_EQ(hi->literal, "10");
}

TEST_F(ParserPrefixTest, Range_Identifiers) {
  auto r = ExprResult::from("(start..end)");
  EXPECT_TRUE(r.errors.empty());
  auto *n = r.as<RangeExprNode>();
  ASSERT_NE(n, nullptr);
  auto *lo = std::get_if<IdentifierNode>(&n->low->data);
  auto *hi = std::get_if<IdentifierNode>(&n->high->data);
  ASSERT_NE(lo, nullptr);
  ASSERT_NE(hi, nullptr);
  EXPECT_EQ(lo->name, "start");
  EXPECT_EQ(hi->name, "end");
}

// ── Array literals ────────────────────────────────────────────────────────────

TEST_F(ParserPrefixTest, Array_Empty) {
  auto r = ExprResult::from("[]");
  EXPECT_TRUE(r.errors.empty());
  auto *n = r.as<ArrayLiteralNode>();
  ASSERT_NE(n, nullptr);
  EXPECT_TRUE(n->elements.empty());
}

TEST_F(ParserPrefixTest, Array_SingleElement) {
  auto r = ExprResult::from("[42]");
  EXPECT_TRUE(r.errors.empty());
  auto *n = r.as<ArrayLiteralNode>();
  ASSERT_NE(n, nullptr);
  ASSERT_EQ(n->elements.size(), 1u);
  auto *elem = std::get_if<IntegerLiteralNode>(&n->elements[0]->data);
  ASSERT_NE(elem, nullptr);
  EXPECT_EQ(elem->literal, "42");
}

TEST_F(ParserPrefixTest, Array_MultipleElements) {
  auto r = ExprResult::from("[1, 2, 3]");
  EXPECT_TRUE(r.errors.empty());
  auto *n = r.as<ArrayLiteralNode>();
  ASSERT_NE(n, nullptr);
  EXPECT_EQ(n->elements.size(), 3u);
}

TEST_F(ParserPrefixTest, Array_TrailingComma) {
  auto r = ExprResult::from("[1, 2,]");
  EXPECT_TRUE(r.errors.empty());
  auto *n = r.as<ArrayLiteralNode>();
  ASSERT_NE(n, nullptr);
  EXPECT_EQ(n->elements.size(), 2u);
}

TEST_F(ParserPrefixTest, Array_IdentifierElements) {
  auto r = ExprResult::from("[a, b, c]");
  EXPECT_TRUE(r.errors.empty());
  auto *n = r.as<ArrayLiteralNode>();
  ASSERT_NE(n, nullptr);
  ASSERT_EQ(n->elements.size(), 3u);
  auto *a = std::get_if<IdentifierNode>(&n->elements[0]->data);
  auto *b = std::get_if<IdentifierNode>(&n->elements[1]->data);
  auto *c = std::get_if<IdentifierNode>(&n->elements[2]->data);
  ASSERT_NE(a, nullptr); EXPECT_EQ(a->name, "a");
  ASSERT_NE(b, nullptr); EXPECT_EQ(b->name, "b");
  ASSERT_NE(c, nullptr); EXPECT_EQ(c->name, "c");
}

// Nested array: [[1, 2], [3, 4]]
TEST_F(ParserPrefixTest, Array_Nested) {
  auto r = ExprResult::from("[[1, 2], [3, 4]]");
  EXPECT_TRUE(r.errors.empty());
  auto *n = r.as<ArrayLiteralNode>();
  ASSERT_NE(n, nullptr);
  ASSERT_EQ(n->elements.size(), 2u);
  auto *inner0 = std::get_if<ArrayLiteralNode>(&n->elements[0]->data);
  auto *inner1 = std::get_if<ArrayLiteralNode>(&n->elements[1]->data);
  ASSERT_NE(inner0, nullptr);
  ASSERT_NE(inner1, nullptr);
  EXPECT_EQ(inner0->elements.size(), 2u);
  EXPECT_EQ(inner1->elements.size(), 2u);
}

// ── Import expression ─────────────────────────────────────────────────────────

TEST_F(ParserPrefixTest, ImportExpr_Simple) {
  auto r = ExprResult::from("import \"std/io\"");
  EXPECT_TRUE(r.errors.empty());
  auto *n = r.as<ImportExprNode>();
  ASSERT_NE(n, nullptr);
  EXPECT_EQ(n->path, "std/io");
}

TEST_F(ParserPrefixTest, ImportExpr_NestedPath) {
  auto r = ExprResult::from("import \"mega/long/mathematics\"");
  EXPECT_TRUE(r.errors.empty());
  auto *n = r.as<ImportExprNode>();
  ASSERT_NE(n, nullptr);
  EXPECT_EQ(n->path, "mega/long/mathematics");
}

// ── Error cases ────────────────────────────────────────────────────────────────

TEST_F(ParserPrefixTest, Error_UnknownPrefixToken) {
  auto r = ExprResult::from("?");
  EXPECT_FALSE(r.errors.empty());
  // expr may be nullptr — that is acceptable
}

// =============================================================================
// BlockResult — helper for block-level unit tests.
//
// Calls parse_block() directly after priming the lexer, so tests can inspect
// the statement list without wrapping the block in a declaration.
// =============================================================================

struct BlockResult {
  FileSet            fs;
  NodePtr            block;
  std::vector<Error> errors;

  static BlockResult from(const std::string &source) {
    BlockResult r;
    r.fs.add_file(File::from_source("test.rg", source));
    Parser p(r.fs);
    p.init_for_file(r.fs.files[0].get());
    r.block  = p.parse_block();
    r.errors = p.errors.errors;
    return r;
  }

  const BlockNode *as_block() const {
    if (!block) return nullptr;
    return std::get_if<BlockNode>(&block->data);
  }

  // Unwrap the Nth statement inside the block as type T.
  template <typename T>
  const T *stmt_as(size_t index) const {
    auto *blk = as_block();
    if (!blk || index >= blk->stmts.size()) return nullptr;
    return std::get_if<T>(&blk->stmts.at(index)->data);
  }

  size_t stmt_count() const {
    auto *blk = as_block();
    return blk ? blk->stmts.size() : 0u;
  }
};

// =============================================================================
// Phase — Group 2: parse_block / parse_statement and all statement forms
// =============================================================================

class ParserBlockTest : public ::testing::Test {};

// ── Block structure ───────────────────────────────────────────────────────────

TEST_F(ParserBlockTest, Block_Empty) {
  auto r = BlockResult::from("{}");
  EXPECT_TRUE(r.errors.empty());
  ASSERT_NE(r.as_block(), nullptr);
  EXPECT_EQ(r.stmt_count(), 0u);
}

TEST_F(ParserBlockTest, Block_SingleExpressionStatement) {
  auto r = BlockResult::from("{ 42 }");
  EXPECT_TRUE(r.errors.empty());
  EXPECT_EQ(r.stmt_count(), 1u);
  auto *n = r.stmt_as<IntegerLiteralNode>(0);
  ASSERT_NE(n, nullptr);
  EXPECT_EQ(n->literal, "42");
}

TEST_F(ParserBlockTest, Block_MultipleStatements) {
  auto r = BlockResult::from("{\n  42\n  true\n}");
  EXPECT_TRUE(r.errors.empty());
  EXPECT_EQ(r.stmt_count(), 2u);
  EXPECT_NE(r.stmt_as<IntegerLiteralNode>(0), nullptr);
  EXPECT_NE(r.stmt_as<BoolLiteralNode>(1), nullptr);
}

TEST_F(ParserBlockTest, Block_StatementsOnOneLine) {
  // Newlines are the separator; two statements on the same logical line are
  // handled by the statement loop — each parse_statement() consumes one item.
  auto r = BlockResult::from("{\n  x := 1\n  y := 2\n}");
  EXPECT_TRUE(r.errors.empty());
  EXPECT_EQ(r.stmt_count(), 2u);
}

// ── return / break / next ─────────────────────────────────────────────────────

TEST_F(ParserBlockTest, Statement_Return_NoValue) {
  auto r = BlockResult::from("{ return }");
  EXPECT_TRUE(r.errors.empty());
  auto *n = r.stmt_as<ReturnNode>(0);
  ASSERT_NE(n, nullptr);
  EXPECT_TRUE(n->values.empty());
}

TEST_F(ParserBlockTest, Statement_Return_WithValue) {
  auto r = BlockResult::from("{ return 42 }");
  EXPECT_TRUE(r.errors.empty());
  auto *n = r.stmt_as<ReturnNode>(0);
  ASSERT_NE(n, nullptr);
  ASSERT_EQ(n->values.size(), 1u);
  auto *val = std::get_if<IntegerLiteralNode>(&n->values[0]->data);
  ASSERT_NE(val, nullptr);
  EXPECT_EQ(val->literal, "42");
}

TEST_F(ParserBlockTest, Statement_Return_MultipleValues) {
  auto r = BlockResult::from("{ return 1, 2 }");
  EXPECT_TRUE(r.errors.empty());
  auto *n = r.stmt_as<ReturnNode>(0);
  ASSERT_NE(n, nullptr);
  EXPECT_EQ(n->values.size(), 2u);
}

TEST_F(ParserBlockTest, Statement_Break_NoValue) {
  auto r = BlockResult::from("{ break }");
  EXPECT_TRUE(r.errors.empty());
  auto *n = r.stmt_as<BreakNode>(0);
  ASSERT_NE(n, nullptr);
  EXPECT_TRUE(n->values.empty());
}

TEST_F(ParserBlockTest, Statement_Break_WithValue) {
  auto r = BlockResult::from("{ break 99 }");
  EXPECT_TRUE(r.errors.empty());
  auto *n = r.stmt_as<BreakNode>(0);
  ASSERT_NE(n, nullptr);
  ASSERT_EQ(n->values.size(), 1u);
}

TEST_F(ParserBlockTest, Statement_Next) {
  auto r = BlockResult::from("{ next }");
  EXPECT_TRUE(r.errors.empty());
  EXPECT_NE(r.stmt_as<NextNode>(0), nullptr);
}

// ── VarDecl ───────────────────────────────────────────────────────────────────

TEST_F(ParserBlockTest, Statement_VarDecl_NoInit) {
  auto r = BlockResult::from("{ x Int }");
  EXPECT_TRUE(r.errors.empty());
  auto *n = r.stmt_as<VarDeclNode>(0);
  ASSERT_NE(n, nullptr);
  EXPECT_EQ(n->name.name, "x");
  ASSERT_TRUE(n->type.has_value());
  auto *type_id = std::get_if<IdentifierNode>(&(*n->type)->data);
  ASSERT_NE(type_id, nullptr);
  EXPECT_EQ(type_id->name, "Int");
  EXPECT_FALSE(n->init.has_value());
}

TEST_F(ParserBlockTest, Statement_VarDecl_WithInit) {
  auto r = BlockResult::from("{ x Int = 42 }");
  EXPECT_TRUE(r.errors.empty());
  auto *n = r.stmt_as<VarDeclNode>(0);
  ASSERT_NE(n, nullptr);
  EXPECT_EQ(n->name.name, "x");
  ASSERT_TRUE(n->init.has_value());
  auto *init = std::get_if<IntegerLiteralNode>(&(*n->init)->data);
  ASSERT_NE(init, nullptr);
  EXPECT_EQ(init->literal, "42");
}

TEST_F(ParserBlockTest, Statement_VarDecl_FnType) {
  // "fn" after an identifier is an unambiguous type-start → VarDecl.
  // The Signature grammar requires named parameters, so "fn(x Int) Bool"
  // is the correct form — "fn(Int) Bool" (no name) is a syntax error.
  auto r = BlockResult::from("{ callback fn(x Int) Bool }");
  EXPECT_TRUE(r.errors.empty());
  auto *n = r.stmt_as<VarDeclNode>(0);
  ASSERT_NE(n, nullptr);
  EXPECT_EQ(n->name.name, "callback");
  ASSERT_TRUE(n->type.has_value());
  EXPECT_NE(std::get_if<FuncTypeNode>(&(*n->type)->data), nullptr);
}

// ── DeclAssign (:=) ───────────────────────────────────────────────────────────

TEST_F(ParserBlockTest, Statement_DeclAssign_Single) {
  auto r = BlockResult::from("{ x := 42 }");
  EXPECT_TRUE(r.errors.empty());
  auto *n = r.stmt_as<DeclAssignNode>(0);
  ASSERT_NE(n, nullptr);
  ASSERT_EQ(n->targets.identifiers.size(), 1u);
  EXPECT_EQ(n->targets.identifiers[0].name, "x");
  auto *val = std::get_if<IntegerLiteralNode>(&n->value->data);
  ASSERT_NE(val, nullptr);
  EXPECT_EQ(val->literal, "42");
}

TEST_F(ParserBlockTest, Statement_DeclAssign_Multi) {
  auto r = BlockResult::from("{ x, y := true }");
  EXPECT_TRUE(r.errors.empty());
  auto *n = r.stmt_as<DeclAssignNode>(0);
  ASSERT_NE(n, nullptr);
  ASSERT_EQ(n->targets.identifiers.size(), 2u);
  EXPECT_EQ(n->targets.identifiers[0].name, "x");
  EXPECT_EQ(n->targets.identifiers[1].name, "y");
}

// ── Assignment (=  +=  -=  *=  /=) ───────────────────────────────────────────

TEST_F(ParserBlockTest, Statement_Assign_Plain) {
  auto r = BlockResult::from("{ x = 10 }");
  EXPECT_TRUE(r.errors.empty());
  auto *n = r.stmt_as<AssignNode>(0);
  ASSERT_NE(n, nullptr);
  EXPECT_EQ(n->op, Token::Kind::Assignment);
  ASSERT_EQ(n->targets.size(), 1u);
  auto *target = std::get_if<IdentifierNode>(&n->targets[0]->data);
  ASSERT_NE(target, nullptr);
  EXPECT_EQ(target->name, "x");
  ASSERT_EQ(n->values.size(), 1u);
  auto *val = std::get_if<IntegerLiteralNode>(&n->values[0]->data);
  ASSERT_NE(val, nullptr);
  EXPECT_EQ(val->literal, "10");
}

TEST_F(ParserBlockTest, Statement_Assign_Add) {
  auto r = BlockResult::from("{ x += 1 }");
  EXPECT_TRUE(r.errors.empty());
  auto *n = r.stmt_as<AssignNode>(0);
  ASSERT_NE(n, nullptr);
  EXPECT_EQ(n->op, Token::Kind::AddAssignment);
}

TEST_F(ParserBlockTest, Statement_Assign_Sub) {
  auto r = BlockResult::from("{ x -= 2 }");
  EXPECT_TRUE(r.errors.empty());
  auto *n = r.stmt_as<AssignNode>(0);
  ASSERT_NE(n, nullptr);
  EXPECT_EQ(n->op, Token::Kind::SubAssignment);
}

TEST_F(ParserBlockTest, Statement_Assign_Mul) {
  auto r = BlockResult::from("{ x *= 3 }");
  EXPECT_TRUE(r.errors.empty());
  auto *n = r.stmt_as<AssignNode>(0);
  ASSERT_NE(n, nullptr);
  EXPECT_EQ(n->op, Token::Kind::MulAssignment);
}

TEST_F(ParserBlockTest, Statement_Assign_Div) {
  auto r = BlockResult::from("{ x /= 4 }");
  EXPECT_TRUE(r.errors.empty());
  auto *n = r.stmt_as<AssignNode>(0);
  ASSERT_NE(n, nullptr);
  EXPECT_EQ(n->op, Token::Kind::DivAssignment);
}

TEST_F(ParserBlockTest, Statement_Assign_Multi_Target) {
  auto r = BlockResult::from("{ x, y = true, false }");
  EXPECT_TRUE(r.errors.empty());
  auto *n = r.stmt_as<AssignNode>(0);
  ASSERT_NE(n, nullptr);
  EXPECT_EQ(n->op, Token::Kind::Assignment);
  EXPECT_EQ(n->targets.size(), 2u);
  EXPECT_EQ(n->values.size(), 2u);
}

// ── Increment / Decrement ─────────────────────────────────────────────────────

TEST_F(ParserBlockTest, Statement_Increment) {
  auto r = BlockResult::from("{ x++ }");
  EXPECT_TRUE(r.errors.empty());
  auto *n = r.stmt_as<IncrementNode>(0);
  ASSERT_NE(n, nullptr);
  auto *operand = std::get_if<IdentifierNode>(&n->operand->data);
  ASSERT_NE(operand, nullptr);
  EXPECT_EQ(operand->name, "x");
}

TEST_F(ParserBlockTest, Statement_Decrement) {
  auto r = BlockResult::from("{ x-- }");
  EXPECT_TRUE(r.errors.empty());
  auto *n = r.stmt_as<DecrementNode>(0);
  ASSERT_NE(n, nullptr);
  auto *operand = std::get_if<IdentifierNode>(&n->operand->data);
  ASSERT_NE(operand, nullptr);
  EXPECT_EQ(operand->name, "x");
}

// ── Mixed block ───────────────────────────────────────────────────────────────

TEST_F(ParserBlockTest, Block_Mixed_Statements) {
  auto r = BlockResult::from(
      "{\n"
      "  x Int = 0\n"
      "  x += 1\n"
      "  x++\n"
      "  return x\n"
      "}");
  EXPECT_TRUE(r.errors.empty());
  EXPECT_EQ(r.stmt_count(), 4u);
  EXPECT_NE(r.stmt_as<VarDeclNode>(0),    nullptr);
  EXPECT_NE(r.stmt_as<AssignNode>(1),     nullptr);
  EXPECT_NE(r.stmt_as<IncrementNode>(2),  nullptr);
  EXPECT_NE(r.stmt_as<ReturnNode>(3),     nullptr);
}

// =============================================================================
// Phase — Group 3: parse_if_expr / parse_case_arm / parse_switch_expr
// =============================================================================

class ParserCompoundTest : public ::testing::Test {};

// ── if expression ─────────────────────────────────────────────────────────────

TEST_F(ParserCompoundTest, If_NoElse) {
  auto r = ExprResult::from("if true { 42 }");
  EXPECT_TRUE(r.errors.empty());
  auto *n = r.as<IfExprNode>();
  ASSERT_NE(n, nullptr);

  auto *cond = std::get_if<BoolLiteralNode>(&n->condition->data);
  ASSERT_NE(cond, nullptr);
  EXPECT_EQ(cond->literal, "true");

  auto *blk = std::get_if<BlockNode>(&n->then_block->data);
  ASSERT_NE(blk, nullptr);
  ASSERT_EQ(blk->stmts.size(), 1u);
  EXPECT_NE(std::get_if<IntegerLiteralNode>(&blk->stmts[0]->data), nullptr);

  EXPECT_FALSE(n->else_block.has_value());
}

TEST_F(ParserCompoundTest, If_WithElse) {
  auto r = ExprResult::from("if x { 1 } else { 2 }");
  EXPECT_TRUE(r.errors.empty());
  auto *n = r.as<IfExprNode>();
  ASSERT_NE(n, nullptr);

  EXPECT_NE(std::get_if<IdentifierNode>(&n->condition->data), nullptr);

  ASSERT_TRUE(n->else_block.has_value());
  // else_block must be a plain BlockNode — "else if" is not valid
  auto *else_blk = std::get_if<BlockNode>(&(*n->else_block)->data);
  ASSERT_NE(else_blk, nullptr);
  ASSERT_EQ(else_blk->stmts.size(), 1u);
  auto *val = std::get_if<IntegerLiteralNode>(&else_blk->stmts[0]->data);
  ASSERT_NE(val, nullptr);
  EXPECT_EQ(val->literal, "2");
}

// Condition can be any expression — here a unary op.
TEST_F(ParserCompoundTest, If_Condition_Unary) {
  auto r = ExprResult::from("if !done { 0 }");
  EXPECT_TRUE(r.errors.empty());
  auto *n = r.as<IfExprNode>();
  ASSERT_NE(n, nullptr);
  EXPECT_NE(std::get_if<UnaryExprNode>(&n->condition->data), nullptr);
}

// Multi-statement then-block
TEST_F(ParserCompoundTest, If_MultiStatement_Body) {
  auto r = ExprResult::from("if x {\n  y := 1\n  return y\n}");
  EXPECT_TRUE(r.errors.empty());
  auto *n = r.as<IfExprNode>();
  ASSERT_NE(n, nullptr);
  auto *blk = std::get_if<BlockNode>(&n->then_block->data);
  ASSERT_NE(blk, nullptr);
  EXPECT_EQ(blk->stmts.size(), 2u);
}

// ── switch expression ─────────────────────────────────────────────────────────

TEST_F(ParserCompoundTest, Switch_SingleArm_ExprBody) {
  auto r = ExprResult::from("switch x {\ncase 1: 42\n}");
  EXPECT_TRUE(r.errors.empty());
  auto *n = r.as<SwitchExprNode>();
  ASSERT_NE(n, nullptr);

  // subject
  EXPECT_NE(std::get_if<IdentifierNode>(&n->subject->data), nullptr);

  // one arm, expression body
  ASSERT_EQ(n->arms.size(), 1u);
  auto *pat = std::get_if<IntegerLiteralNode>(&n->arms[0].pattern->data);
  ASSERT_NE(pat, nullptr);
  EXPECT_EQ(pat->literal, "1");
  EXPECT_NE(std::get_if<IntegerLiteralNode>(&n->arms[0].body->data), nullptr);

  EXPECT_FALSE(n->else_body.has_value());
}

TEST_F(ParserCompoundTest, Switch_MultipleArms) {
  auto r = ExprResult::from("switch x {\ncase 1: 10\ncase 2: 20\n}");
  EXPECT_TRUE(r.errors.empty());
  auto *n = r.as<SwitchExprNode>();
  ASSERT_NE(n, nullptr);
  EXPECT_EQ(n->arms.size(), 2u);
}

TEST_F(ParserCompoundTest, Switch_WithElseArm_ExprBody) {
  auto r = ExprResult::from("switch x {\ncase 1: 10\nelse: 99\n}");
  EXPECT_TRUE(r.errors.empty());
  auto *n = r.as<SwitchExprNode>();
  ASSERT_NE(n, nullptr);
  ASSERT_EQ(n->arms.size(), 1u);
  ASSERT_TRUE(n->else_body.has_value());
  auto *els = std::get_if<IntegerLiteralNode>(&(*n->else_body)->data);
  ASSERT_NE(els, nullptr);
  EXPECT_EQ(els->literal, "99");
}

TEST_F(ParserCompoundTest, Switch_ArmWithBlockBody) {
  auto r = ExprResult::from("switch x {\ncase 1: {\n  return 42\n}\n}");
  EXPECT_TRUE(r.errors.empty());
  auto *n = r.as<SwitchExprNode>();
  ASSERT_NE(n, nullptr);
  ASSERT_EQ(n->arms.size(), 1u);
  EXPECT_NE(std::get_if<BlockNode>(&n->arms[0].body->data), nullptr);
}

TEST_F(ParserCompoundTest, Switch_ElseArmWithBlockBody) {
  auto r = ExprResult::from("switch x {\ncase 1: 10\nelse: {\n  return 99\n}\n}");
  EXPECT_TRUE(r.errors.empty());
  auto *n = r.as<SwitchExprNode>();
  ASSERT_NE(n, nullptr);
  ASSERT_TRUE(n->else_body.has_value());
  EXPECT_NE(std::get_if<BlockNode>(&(*n->else_body)->data), nullptr);
}

// switch inside a block (statement context)
TEST_F(ParserCompoundTest, Switch_InsideBlock) {
  auto r = BlockResult::from(
      "{\n"
      "  result := switch x {\n"
      "    case 1: true\n"
      "    else: false\n"
      "  }\n"
      "}");
  EXPECT_TRUE(r.errors.empty());
  // The DeclAssign wraps a SwitchExprNode as its value.
  auto *decl = r.stmt_as<DeclAssignNode>(0);
  ASSERT_NE(decl, nullptr);
  EXPECT_NE(std::get_if<SwitchExprNode>(&decl->value->data), nullptr);
}

// if used as a value in an assignment (expression context inside a block)
TEST_F(ParserCompoundTest, If_AsValue_InDeclAssign) {
  auto r = BlockResult::from("{ x := if true { 1 } else { 2 } }");
  EXPECT_TRUE(r.errors.empty());
  auto *decl = r.stmt_as<DeclAssignNode>(0);
  ASSERT_NE(decl, nullptr);
  EXPECT_NE(std::get_if<IfExprNode>(&decl->value->data), nullptr);
}

// =============================================================================
// Phase — parse_for_expr
// =============================================================================

class ParserForTest : public ::testing::Test {};

// helpers -----------------------------------------------------------------

static const ForExprNode *for_expr(const ExprResult &r) {
  return r.as<ForExprNode>();
}

// ── Infinite loop: no mode, no accumulator ───────────────────────────────────

TEST_F(ParserForTest, For_Infinite) {
  auto r = ExprResult::from("for { }");
  EXPECT_TRUE(r.errors.empty());
  auto *n = for_expr(r);
  ASSERT_NE(n, nullptr);
  EXPECT_FALSE(n->mode.has_value());
  EXPECT_FALSE(n->accumulator.has_value());
  EXPECT_NE(std::get_if<BlockNode>(&n->body->data), nullptr);
}

// ── Infinite loop with accumulator pipe ──────────────────────────────────────

TEST_F(ParserForTest, For_Infinite_WithAccumulator) {
  auto r = ExprResult::from("for |acc| { }");
  EXPECT_TRUE(r.errors.empty());
  auto *n = for_expr(r);
  ASSERT_NE(n, nullptr);
  EXPECT_FALSE(n->mode.has_value());
  ASSERT_TRUE(n->accumulator.has_value());
  EXPECT_EQ(n->accumulator->name, "acc");
}

// ── Bare condition (while-style) ─────────────────────────────────────────────

TEST_F(ParserForTest, For_Condition_Identifier) {
  auto r = ExprResult::from("for running { }");
  EXPECT_TRUE(r.errors.empty());
  auto *n = for_expr(r);
  ASSERT_NE(n, nullptr);
  ASSERT_TRUE(n->mode.has_value());
  EXPECT_NE(std::get_if<IdentifierNode>(&(*n->mode)->data), nullptr);
}

// NOTE: for-condition tests that use binary operators (e.g. "for x > 0 { }")
// and accumulator-after-expression tests (e.g. "for running |result| { }")
// are deferred until parse_infix is implemented.  With the stub, parse_infix
// advances past one infix token and returns the LHS unchanged, which either
// leaves a stray literal before "{" (binary-op case) or consumes the "|"
// that was meant as the accumulator pipe delimiter (pipe case).

// ── Range clause ─────────────────────────────────────────────────────────────

TEST_F(ParserForTest, For_Range_SingleVar) {
  auto r = ExprResult::from("for item : items { }");
  EXPECT_TRUE(r.errors.empty());
  auto *n = for_expr(r);
  ASSERT_NE(n, nullptr);
  ASSERT_TRUE(n->mode.has_value());
  auto *range = std::get_if<ForRangeClauseNode>(&(*n->mode)->data);
  ASSERT_NE(range, nullptr);
  ASSERT_EQ(range->vars.size(), 1u);
  EXPECT_EQ(range->vars[0].name, "item");
  EXPECT_NE(std::get_if<IdentifierNode>(&range->iterable->data), nullptr);
}

TEST_F(ParserForTest, For_Range_TwoVars) {
  auto r = ExprResult::from("for k, v : map { }");
  EXPECT_TRUE(r.errors.empty());
  auto *n = for_expr(r);
  ASSERT_NE(n, nullptr);
  ASSERT_TRUE(n->mode.has_value());
  auto *range = std::get_if<ForRangeClauseNode>(&(*n->mode)->data);
  ASSERT_NE(range, nullptr);
  ASSERT_EQ(range->vars.size(), 2u);
  EXPECT_EQ(range->vars[0].name, "k");
  EXPECT_EQ(range->vars[1].name, "v");
}

// NOTE: For_Range_WithAccumulator ("for item : items |sum| { }") is deferred:
// parse_expression() for the iterable "items" has infix_bp(|)=60 so the stub
// consumes "|", leaving "sum" as current instead of the pipe delimiter.

// ── Iterator clause ──────────────────────────────────────────────────────────

TEST_F(ParserForTest, For_Iterator_Basic) {
  // Condition is "i" (bare identifier) rather than "i < 10": the comparison
  // uses a binary operator which the parse_infix stub would mishandle.
  // Structure (DeclAssign init / condition / IncrementNode update) is fully
  // exercised; only the condition expression is trivially simple for now.
  auto r = ExprResult::from("for i := 0 ; i ; i++ { }");
  EXPECT_TRUE(r.errors.empty());
  auto *n = for_expr(r);
  ASSERT_NE(n, nullptr);
  ASSERT_TRUE(n->mode.has_value());
  auto *iter = std::get_if<ForIterClauseNode>(&(*n->mode)->data);
  ASSERT_NE(iter, nullptr);

  // init must be a DeclAssignNode for "i := 0"
  auto *init = std::get_if<DeclAssignNode>(&iter->init->data);
  ASSERT_NE(init, nullptr);
  ASSERT_EQ(init->targets.identifiers.size(), 1u);
  EXPECT_EQ(init->targets.identifiers[0].name, "i");
  auto *init_val = std::get_if<IntegerLiteralNode>(&init->value->data);
  ASSERT_NE(init_val, nullptr);
  EXPECT_EQ(init_val->literal, "0");

  // condition — parse_infix is a stub; just check it is non-null
  EXPECT_NE(iter->condition.get(), nullptr);

  // update must be an IncrementNode for "i++"
  auto *update = std::get_if<IncrementNode>(&iter->update->data);
  ASSERT_NE(update, nullptr);
  auto *upd_id = std::get_if<IdentifierNode>(&update->operand->data);
  ASSERT_NE(upd_id, nullptr);
  EXPECT_EQ(upd_id->name, "i");
}

TEST_F(ParserForTest, For_Iterator_WithAddAssign) {
  auto r = ExprResult::from("for i := 0 ; i ; i += 2 { }");
  EXPECT_TRUE(r.errors.empty());
  auto *n = for_expr(r);
  ASSERT_NE(n, nullptr);
  auto *iter = std::get_if<ForIterClauseNode>(&(*n->mode)->data);
  ASSERT_NE(iter, nullptr);
  auto *update = std::get_if<AssignNode>(&iter->update->data);
  ASSERT_NE(update, nullptr);
  EXPECT_EQ(update->op, Token::Kind::AddAssignment);
}

// ── for as a statement value ──────────────────────────────────────────────────

TEST_F(ParserForTest, For_AsValueInBlock) {
  auto r = BlockResult::from("{ result := for item : items { item } }");
  EXPECT_TRUE(r.errors.empty());
  auto *decl = r.stmt_as<DeclAssignNode>(0);
  ASSERT_NE(decl, nullptr);
  EXPECT_NE(std::get_if<ForExprNode>(&decl->value->data), nullptr);
}

// =============================================================================
// parse_func_expr
// =============================================================================

class ParserFuncExprTest : public ::testing::Test {};

// ── helpers ───────────────────────────────────────────────────────────────────

static const FuncExprNode *func_expr(const ExprResult &r) {
  return r.as<FuncExprNode>();
}

// ── basic forms ───────────────────────────────────────────────────────────────

TEST_F(ParserFuncExprTest, FuncExpr_NoParamsNoReturn) {
  auto r = ExprResult::from("fn() { }");
  EXPECT_TRUE(r.errors.empty());
  auto *n = func_expr(r);
  ASSERT_NE(n, nullptr);
  EXPECT_FALSE(n->generic.has_value());
  EXPECT_TRUE(n->signature.params.empty());
  EXPECT_TRUE(n->signature.returns.empty());
  EXPECT_NE(std::get_if<BlockNode>(&n->body->data), nullptr);
}

TEST_F(ParserFuncExprTest, FuncExpr_SingleParam_SingleReturn) {
  auto r = ExprResult::from("fn(x Int) Bool { return false }");
  EXPECT_TRUE(r.errors.empty());
  auto *n = func_expr(r);
  ASSERT_NE(n, nullptr);

  ASSERT_EQ(n->signature.params.size(), 1u);
  const auto &p = n->signature.params[0];
  ASSERT_EQ(p.names.identifiers.size(), 1u);
  EXPECT_EQ(p.names.identifiers[0].name, "x");
  auto *pt = std::get_if<IdentifierNode>(&p.type->data);
  ASSERT_NE(pt, nullptr);
  EXPECT_EQ(pt->name, "Int");
  EXPECT_FALSE(p.is_variadic);

  ASSERT_EQ(n->signature.returns.size(), 1u);
  auto *rt = std::get_if<IdentifierNode>(&n->signature.returns[0]->data);
  ASSERT_NE(rt, nullptr);
  EXPECT_EQ(rt->name, "Bool");
}

// Multiple names in one parameter group: "x, y Int" → one ParameterNode
TEST_F(ParserFuncExprTest, FuncExpr_MultiName_Param) {
  auto r = ExprResult::from("fn(x, y Int) { }");
  EXPECT_TRUE(r.errors.empty());
  auto *n = func_expr(r);
  ASSERT_NE(n, nullptr);

  ASSERT_EQ(n->signature.params.size(), 1u);
  const auto &p = n->signature.params[0];
  ASSERT_EQ(p.names.identifiers.size(), 2u);
  EXPECT_EQ(p.names.identifiers[0].name, "x");
  EXPECT_EQ(p.names.identifiers[1].name, "y");
  EXPECT_TRUE(n->signature.returns.empty());
}

// Multiple parameter groups
TEST_F(ParserFuncExprTest, FuncExpr_MultiParam_Groups) {
  auto r = ExprResult::from("fn(x Int, b Bool) { }");
  EXPECT_TRUE(r.errors.empty());
  auto *n = func_expr(r);
  ASSERT_NE(n, nullptr);
  EXPECT_EQ(n->signature.params.size(), 2u);
}

// Multiple return types
TEST_F(ParserFuncExprTest, FuncExpr_MultiReturn) {
  auto r = ExprResult::from("fn(x Int) Int, Bool { }");
  EXPECT_TRUE(r.errors.empty());
  auto *n = func_expr(r);
  ASSERT_NE(n, nullptr);
  ASSERT_EQ(n->signature.returns.size(), 2u);
  auto *r0 = std::get_if<IdentifierNode>(&n->signature.returns[0]->data);
  auto *r1 = std::get_if<IdentifierNode>(&n->signature.returns[1]->data);
  ASSERT_NE(r0, nullptr); EXPECT_EQ(r0->name, "Int");
  ASSERT_NE(r1, nullptr); EXPECT_EQ(r1->name, "Bool");
}

// Variadic parameter
TEST_F(ParserFuncExprTest, FuncExpr_Variadic) {
  auto r = ExprResult::from("fn(args ...Int) { }");
  EXPECT_TRUE(r.errors.empty());
  auto *n = func_expr(r);
  ASSERT_NE(n, nullptr);
  ASSERT_EQ(n->signature.params.size(), 1u);
  EXPECT_TRUE(n->signature.params[0].is_variadic);
}

// ── generic ───────────────────────────────────────────────────────────────────

TEST_F(ParserFuncExprTest, FuncExpr_Generic_Single) {
  auto r = ExprResult::from("fn |T| (x T) T { }");
  EXPECT_TRUE(r.errors.empty());
  auto *n = func_expr(r);
  ASSERT_NE(n, nullptr);

  ASSERT_TRUE(n->generic.has_value());
  ASSERT_EQ(n->generic->type_params.size(), 1u);
  auto *tp = std::get_if<IdentifierNode>(&n->generic->type_params[0]->data);
  ASSERT_NE(tp, nullptr);
  EXPECT_EQ(tp->name, "T");

  ASSERT_EQ(n->signature.params.size(), 1u);
  auto *pt = std::get_if<IdentifierNode>(&n->signature.params[0].type->data);
  ASSERT_NE(pt, nullptr);
  EXPECT_EQ(pt->name, "T");
}

TEST_F(ParserFuncExprTest, FuncExpr_Generic_Multi) {
  auto r = ExprResult::from("fn |K, V| (k K, v V) { }");
  EXPECT_TRUE(r.errors.empty());
  auto *n = func_expr(r);
  ASSERT_NE(n, nullptr);
  ASSERT_TRUE(n->generic.has_value());
  EXPECT_EQ(n->generic->type_params.size(), 2u);
}

// ── body content ─────────────────────────────────────────────────────────────

TEST_F(ParserFuncExprTest, FuncExpr_Body_MultiStatement) {
  auto r = ExprResult::from("fn(x Int) Int {\n  y := x\n  return y\n}");
  EXPECT_TRUE(r.errors.empty());
  auto *n = func_expr(r);
  ASSERT_NE(n, nullptr);
  auto *blk = std::get_if<BlockNode>(&n->body->data);
  ASSERT_NE(blk, nullptr);
  EXPECT_EQ(blk->stmts.size(), 2u);
}

// ── as a value ───────────────────────────────────────────────────────────────

TEST_F(ParserFuncExprTest, FuncExpr_AsValueInDeclAssign) {
  auto r = BlockResult::from("{ add := fn(a, b Int) Int { } }");
  EXPECT_TRUE(r.errors.empty());
  auto *decl = r.stmt_as<DeclAssignNode>(0);
  ASSERT_NE(decl, nullptr);
  EXPECT_NE(std::get_if<FuncExprNode>(&decl->value->data), nullptr);
}

// =============================================================================
// parse_spawn_expr
// =============================================================================

class ParserSpawnTest : public ::testing::Test {};

static const SpawnExprNode *spawn_expr(const ExprResult &r) {
  return r.as<SpawnExprNode>();
}

// ── block body ────────────────────────────────────────────────────────────────

TEST_F(ParserSpawnTest, Spawn_BlockBody) {
  auto r = ExprResult::from("spawn { }");
  EXPECT_TRUE(r.errors.empty());
  auto *n = spawn_expr(r);
  ASSERT_NE(n, nullptr);
  EXPECT_FALSE(n->generic.has_value());
  EXPECT_FALSE(n->pipe.has_value());
  EXPECT_NE(std::get_if<BlockNode>(&n->body->data), nullptr);
}

TEST_F(ParserSpawnTest, Spawn_BlockBody_WithStatements) {
  auto r = ExprResult::from("spawn {\n  x := 1\n  return x\n}");
  EXPECT_TRUE(r.errors.empty());
  auto *n = spawn_expr(r);
  ASSERT_NE(n, nullptr);
  auto *blk = std::get_if<BlockNode>(&n->body->data);
  ASSERT_NE(blk, nullptr);
  EXPECT_EQ(blk->stmts.size(), 2u);
}

// ── identifier body ───────────────────────────────────────────────────────────

TEST_F(ParserSpawnTest, Spawn_IdentifierBody) {
  auto r = ExprResult::from("spawn workerFn");
  EXPECT_TRUE(r.errors.empty());
  auto *n = spawn_expr(r);
  ASSERT_NE(n, nullptr);
  EXPECT_FALSE(n->generic.has_value());
  EXPECT_FALSE(n->pipe.has_value());
  auto *id = std::get_if<IdentifierNode>(&n->body->data);
  ASSERT_NE(id, nullptr);
  EXPECT_EQ(id->name, "workerFn");
}

// ── named task pipe ───────────────────────────────────────────────────────────

TEST_F(ParserSpawnTest, Spawn_WithPipe_Block) {
  auto r = ExprResult::from("spawn |task| { }");
  EXPECT_TRUE(r.errors.empty());
  auto *n = spawn_expr(r);
  ASSERT_NE(n, nullptr);
  EXPECT_FALSE(n->generic.has_value());
  ASSERT_TRUE(n->pipe.has_value());
  EXPECT_EQ(n->pipe->name, "task");
  EXPECT_NE(std::get_if<BlockNode>(&n->body->data), nullptr);
}

// ── generic (typed channel) ───────────────────────────────────────────────────

TEST_F(ParserSpawnTest, Spawn_Generic_BlockBody) {
  auto r = ExprResult::from("|String| spawn { }");
  EXPECT_TRUE(r.errors.empty());
  auto *n = spawn_expr(r);
  ASSERT_NE(n, nullptr);
  ASSERT_TRUE(n->generic.has_value());
  ASSERT_EQ(n->generic->type_params.size(), 1u);
  auto *tp = std::get_if<IdentifierNode>(&n->generic->type_params[0]->data);
  ASSERT_NE(tp, nullptr);
  EXPECT_EQ(tp->name, "String");
  EXPECT_FALSE(n->pipe.has_value());
  EXPECT_NE(std::get_if<BlockNode>(&n->body->data), nullptr);
}

TEST_F(ParserSpawnTest, Spawn_Generic_IdentifierBody) {
  auto r = ExprResult::from("|String| spawn workerFn");
  EXPECT_TRUE(r.errors.empty());
  auto *n = spawn_expr(r);
  ASSERT_NE(n, nullptr);
  ASSERT_TRUE(n->generic.has_value());
  auto *id = std::get_if<IdentifierNode>(&n->body->data);
  ASSERT_NE(id, nullptr);
  EXPECT_EQ(id->name, "workerFn");
}

// ── all three together ────────────────────────────────────────────────────────

TEST_F(ParserSpawnTest, Spawn_Generic_Pipe_Block) {
  auto r = ExprResult::from("|String| spawn |task| { }");
  EXPECT_TRUE(r.errors.empty());
  auto *n = spawn_expr(r);
  ASSERT_NE(n, nullptr);
  ASSERT_TRUE(n->generic.has_value());
  EXPECT_EQ(n->generic->type_params.size(), 1u);
  ASSERT_TRUE(n->pipe.has_value());
  EXPECT_EQ(n->pipe->name, "task");
  EXPECT_NE(std::get_if<BlockNode>(&n->body->data), nullptr);
}

// ── as a value ────────────────────────────────────────────────────────────────

TEST_F(ParserSpawnTest, Spawn_AsValueInDeclAssign) {
  auto r = BlockResult::from("{ ch := |String| spawn { } }");
  EXPECT_TRUE(r.errors.empty());
  auto *decl = r.stmt_as<DeclAssignNode>(0);
  ASSERT_NE(decl, nullptr);
  EXPECT_NE(std::get_if<SpawnExprNode>(&decl->value->data), nullptr);
}

// =============================================================================
// parse_struct_literal
// =============================================================================

class ParserStructLiteralTest : public ::testing::Test {};

static const StructLiteralNode *struct_lit(const ExprResult &r) {
  return r.as<StructLiteralNode>();
}

// ── empty initialiser ─────────────────────────────────────────────────────────

TEST_F(ParserStructLiteralTest, StructLit_Empty) {
  auto r = ExprResult::from("Point { }");
  EXPECT_TRUE(r.errors.empty());
  auto *n = struct_lit(r);
  ASSERT_NE(n, nullptr);
  auto *te = std::get_if<IdentifierNode>(&n->type_expr->data);
  ASSERT_NE(te, nullptr);
  EXPECT_EQ(te->name, "Point");
  EXPECT_TRUE(n->fields.empty());
}

// ── comma-separated fields ────────────────────────────────────────────────────

TEST_F(ParserStructLiteralTest, StructLit_CommaSeparated) {
  auto r = ExprResult::from("Point { x: 0, y: 1 }");
  EXPECT_TRUE(r.errors.empty());
  auto *n = struct_lit(r);
  ASSERT_NE(n, nullptr);
  ASSERT_EQ(n->fields.size(), 2u);

  EXPECT_EQ(n->fields[0].name.name, "x");
  auto *v0 = std::get_if<IntegerLiteralNode>(&n->fields[0].value->data);
  ASSERT_NE(v0, nullptr);
  EXPECT_EQ(v0->literal, "0");

  EXPECT_EQ(n->fields[1].name.name, "y");
  auto *v1 = std::get_if<IntegerLiteralNode>(&n->fields[1].value->data);
  ASSERT_NE(v1, nullptr);
  EXPECT_EQ(v1->literal, "1");
}

// ── newline-separated fields ─────────────────────────────────────────────────

TEST_F(ParserStructLiteralTest, StructLit_NewlineSeparated) {
  auto r = ExprResult::from("Point {\n  x: 0\n  y: 1\n}");
  EXPECT_TRUE(r.errors.empty());
  auto *n = struct_lit(r);
  ASSERT_NE(n, nullptr);
  EXPECT_EQ(n->fields.size(), 2u);
  EXPECT_EQ(n->fields[0].name.name, "x");
  EXPECT_EQ(n->fields[1].name.name, "y");
}

// ── trailing comma ────────────────────────────────────────────────────────────

TEST_F(ParserStructLiteralTest, StructLit_TrailingComma) {
  auto r = ExprResult::from("Point { x: 0, y: 1, }");
  EXPECT_TRUE(r.errors.empty());
  auto *n = struct_lit(r);
  ASSERT_NE(n, nullptr);
  EXPECT_EQ(n->fields.size(), 2u);
}

// ── field values are expressions ──────────────────────────────────────────────

TEST_F(ParserStructLiteralTest, StructLit_ExprValue) {
  auto r = ExprResult::from("Circle { active: true }");
  EXPECT_TRUE(r.errors.empty());
  auto *n = struct_lit(r);
  ASSERT_NE(n, nullptr);
  ASSERT_EQ(n->fields.size(), 1u);
  EXPECT_NE(std::get_if<BoolLiteralNode>(&n->fields[0].value->data), nullptr);
}

// ── nested struct literal as field value ──────────────────────────────────────

TEST_F(ParserStructLiteralTest, StructLit_NestedValue) {
  auto r = ExprResult::from("Line { start: Point { x: 0, y: 0 } }");
  EXPECT_TRUE(r.errors.empty());
  auto *n = struct_lit(r);
  ASSERT_NE(n, nullptr);
  ASSERT_EQ(n->fields.size(), 1u);
  EXPECT_EQ(n->fields[0].name.name, "start");
  auto *inner = std::get_if<StructLiteralNode>(&n->fields[0].value->data);
  ASSERT_NE(inner, nullptr);
  auto *inner_te = std::get_if<IdentifierNode>(&inner->type_expr->data);
  ASSERT_NE(inner_te, nullptr);
  EXPECT_EQ(inner_te->name, "Point");
  EXPECT_EQ(inner->fields.size(), 2u);
}

// ── in statement context ──────────────────────────────────────────────────────

TEST_F(ParserStructLiteralTest, StructLit_InDeclAssign) {
  auto r = BlockResult::from("{ p := Point { x: 1, y: 2 } }");
  EXPECT_TRUE(r.errors.empty());
  auto *decl = r.stmt_as<DeclAssignNode>(0);
  ASSERT_NE(decl, nullptr);
  auto *lit = std::get_if<StructLiteralNode>(&decl->value->data);
  ASSERT_NE(lit, nullptr);
  EXPECT_EQ(lit->fields.size(), 2u);
}

TEST_F(ParserStructLiteralTest, StructLit_InReturn) {
  auto r = BlockResult::from("{ return Point { x: 0, y: 0 } }");
  EXPECT_TRUE(r.errors.empty());
  auto *ret = r.stmt_as<ReturnNode>(0);
  ASSERT_NE(ret, nullptr);
  ASSERT_EQ(ret->values.size(), 1u);
  EXPECT_NE(std::get_if<StructLiteralNode>(&ret->values[0]->data), nullptr);
}

// ── if/switch bodies are not confused with struct initialisers ────────────────

TEST_F(ParserStructLiteralTest, IfBody_NotConsumedAsStructLit) {
  // If `{` had no special treatment the condition "x" followed by "{42}"
  // would be parsed as a struct literal rather than as a block body.
  auto r = ExprResult::from("if x { 42 }");
  EXPECT_TRUE(r.errors.empty());
  auto *n = r.as<IfExprNode>();
  ASSERT_NE(n, nullptr);
  // condition must be a plain identifier, not a struct literal
  EXPECT_NE(std::get_if<IdentifierNode>(&n->condition->data), nullptr);
  // then_block must be a block containing 42
  auto *blk = std::get_if<BlockNode>(&n->then_block->data);
  ASSERT_NE(blk, nullptr);
  EXPECT_EQ(blk->stmts.size(), 1u);
}

TEST_F(ParserStructLiteralTest, ForBody_NotConsumedAsStructLit) {
  auto r = ExprResult::from("for running { }");
  EXPECT_TRUE(r.errors.empty());
  auto *n = r.as<ForExprNode>();
  ASSERT_NE(n, nullptr);
  ASSERT_TRUE(n->mode.has_value());
  EXPECT_NE(std::get_if<IdentifierNode>(&(*n->mode)->data), nullptr);
  EXPECT_NE(std::get_if<BlockNode>(&n->body->data), nullptr);
}

} // namespace mc

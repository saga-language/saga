// Copywrite 2026 Rob Thornton
// SPDX-License-Identifier: MIT

#include "frontend/ast.hpp"

#include <gtest/gtest.h>

namespace saga {

namespace {
// `margin` is what the parser resolves from the closing delimiter; an empty one
// means the text is content verbatim.
std::string inline_form(std::string_view raw) {
  return unescape_string_fragment(raw, std::nullopt);
}

std::string block_form(std::string_view raw, size_t margin) {
  return unescape_string_fragment(raw, margin);
}
} // namespace

TEST(Ast, Unescape_SingleLineLiteral) {
  ASSERT_EQ(inline_form(R"("hello")"), "hello");
  ASSERT_EQ(inline_form(R"("")"), "");
}

TEST(Ast, Unescape_InlineTripleQuoteIsVerbatim) {
  ASSERT_EQ(inline_form(R"("""hello""")"), "hello");
  ASSERT_EQ(inline_form(R"("""""")"), "");
}

TEST(Ast, Unescape_BlockDropsBothLayoutBreaks) {
  ASSERT_EQ(block_form("\"\"\"\na\n\"\"\"", 0), "a");
  ASSERT_EQ(block_form("\"\"\"\r\na\r\n\"\"\"", 0), "a");
  ASSERT_EQ(block_form("\"\"\"\n\"\"\"", 0), "");
  // Only one break on each side — a second is a deliberate blank line.
  ASSERT_EQ(block_form("\"\"\"\n\na\n\"\"\"", 0), "\na");
  ASSERT_EQ(block_form("\"\"\"\na\n\n\"\"\"", 0), "a\n");
}

TEST(Ast, Unescape_BlockStripsMargin) {
  ASSERT_EQ(block_form("\"\"\"\n  a\n  b\n  \"\"\"", 2), "a\nb");
  // Indentation past the margin is content.
  ASSERT_EQ(block_form("\"\"\"\n  a\n    b\n  \"\"\"", 2), "a\n  b");
  // A blank line need not reach the margin.
  ASSERT_EQ(block_form("\"\"\"\n  a\n\n  b\n  \"\"\"", 2), "a\n\nb");
  ASSERT_EQ(block_form("\"\"\"\n\ta\n\t\"\"\"", 1), "a");
}

TEST(Ast, Unescape_BlockNormalisesLineEndings) {
  ASSERT_EQ(block_form("\"\"\"\r\n  a\r\n  b\r\n  \"\"\"", 2), "a\nb");
}

TEST(Ast, Unescape_BlockMarginSkipsEscapedNewline) {
  // `\n` is content, not a line break the margin applies after.
  ASSERT_EQ(block_form("\"\"\"\n  a\\n  b\n  \"\"\"", 2), "a\n  b");
}

TEST(Ast, Unescape_InterpolationPieces) {
  ASSERT_EQ(inline_form(R"("hello {)"), "hello ");
  ASSERT_EQ(inline_form("} then {"), " then ");
  ASSERT_EQ(inline_form(R"(}")"), "");
  ASSERT_EQ(inline_form(R"(} tail")"), " tail");
}

TEST(Ast, Unescape_BlockInterpolationPieces) {
  ASSERT_EQ(block_form("\"\"\"\n  a\n  {", 2), "a\n");
  ASSERT_EQ(block_form("}\n  b\n  \"\"\"", 2), "\nb");
  // A piece opening with `}` resumes its interpolation's line, so its own
  // leading whitespace is content rather than margin.
  ASSERT_EQ(block_form("}  tail\n  b\n  \"\"\"", 2), "  tail\nb");
}

TEST(Ast, Unescape_EscapedBraceIsContent) {
  // `\{` suppresses interpolation, so the lexer hands over a whole literal
  // whose braces must survive stripping.
  ASSERT_EQ(inline_form(R"("a \{ b")"), "a { b");
}

TEST(Ast, Unescape_EscapeSequences) {
  ASSERT_EQ(inline_form(R"("a\nb")"), "a\nb");
  ASSERT_EQ(inline_form(R"("a\rb")"), "a\rb");
  ASSERT_EQ(inline_form(R"("a\tb")"), "a\tb");
  ASSERT_EQ(inline_form(R"("a\\b")"), "a\\b");
  ASSERT_EQ(inline_form(R"("a\"b")"), "a\"b");
  ASSERT_EQ(inline_form(R"("a\}b")"), "a}b");
  // An unknown escape keeps its backslash rather than quietly losing it.
  ASSERT_EQ(inline_form(R"("a\qb")"), "a\\qb");
}

} // namespace saga

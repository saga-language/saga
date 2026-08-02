// Copywrite 2026 Rob Thornton
// SPDX-License-Identifier: MIT

#include "frontend/ast.hpp"

#include <gtest/gtest.h>

namespace saga {

TEST(Ast, Unescape_SingleLineLiteral) {
  ASSERT_EQ(unescape_string_fragment(R"("hello")"), "hello");
  ASSERT_EQ(unescape_string_fragment(R"("")"), "");
}

TEST(Ast, Unescape_MultiLineLiteral) {
  ASSERT_EQ(unescape_string_fragment(R"("""hello""")"), "hello");
  ASSERT_EQ(unescape_string_fragment(R"("""""")"), "");
  ASSERT_EQ(unescape_string_fragment("\"\"\"a\nb\"\"\""), "a\nb");
}

TEST(Ast, Unescape_MultiLineDropsOpeningNewline) {
  ASSERT_EQ(unescape_string_fragment("\"\"\"\na\"\"\""), "a");
  ASSERT_EQ(unescape_string_fragment("\"\"\"\r\na\"\"\""), "a");
  // Only the first — a second newline is a deliberate blank line.
  ASSERT_EQ(unescape_string_fragment("\"\"\"\n\na\"\"\""), "\na");
  // The closing newline is content.
  ASSERT_EQ(unescape_string_fragment("\"\"\"\na\n\"\"\""), "a\n");
  ASSERT_EQ(unescape_string_fragment("\"\"\"\n\"\"\""), "");
  // A single-line string keeps whatever follows its quote.
  ASSERT_EQ(unescape_string_fragment("\"\na\""), "\na");
}

TEST(Ast, Unescape_MultiLineStartPieceDropsOpeningNewline) {
  ASSERT_EQ(unescape_string_fragment("\"\"\"\na {"), "a ");
}

TEST(Ast, Unescape_InterpolationPieces) {
  ASSERT_EQ(unescape_string_fragment(R"("hello {)"), "hello ");
  ASSERT_EQ(unescape_string_fragment("} then {"), " then ");
  ASSERT_EQ(unescape_string_fragment(R"(}")"), "");
  ASSERT_EQ(unescape_string_fragment(R"(} tail")"), " tail");
}

TEST(Ast, Unescape_MultiLineInterpolationPieces) {
  ASSERT_EQ(unescape_string_fragment("\"\"\"a\n{"), "a\n");
  ASSERT_EQ(unescape_string_fragment("}\nb\"\"\""), "\nb");
}

TEST(Ast, Unescape_EscapedBraceIsContent) {
  // `\{` suppresses interpolation, so the lexer hands over a whole literal
  // whose braces must survive stripping.
  ASSERT_EQ(unescape_string_fragment(R"("a \{ b")"), "a { b");
}

TEST(Ast, Unescape_EscapeSequences) {
  ASSERT_EQ(unescape_string_fragment(R"("a\nb")"), "a\nb");
  ASSERT_EQ(unescape_string_fragment(R"("a\tb")"), "a\tb");
  ASSERT_EQ(unescape_string_fragment(R"("a\\b")"), "a\\b");
  ASSERT_EQ(unescape_string_fragment(R"("a\"b")"), "a\"b");
}

} // namespace saga

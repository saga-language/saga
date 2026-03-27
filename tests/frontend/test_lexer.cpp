// Copywrite 2026 Rob Thornton
// SPDX-License-Identifier: MIT

#include "frontend/lexer.hpp"
#include "frontend/token.hpp"

#include "gtest/gtest.h"
#include <gtest/gtest.h>

namespace mc {

// Empty source and special characters
TEST(Lexer, Scan_EmptySource_ReturnsEof) {
  Lexer l;
  auto f = File::from_source("test.txt", "");
  l.init(f.get());
  auto t = l.scan();

  ASSERT_EQ(t.kind, Token::Kind::Eof);
  ASSERT_EQ(t.literal, "");
  ASSERT_EQ(t.offset, 0);
}

TEST(Lexer, Scan_InvalidCharacter_ReturnsInvalid) {
  auto f = File::from_source("test.txt", "@");
  Lexer l;
  l.init(f.get());
  auto t = l.scan();

  ASSERT_EQ(l.error_list.errors.size(), 1);

  ASSERT_EQ(t.kind, Token::Kind::Invalid);
  ASSERT_EQ(t.literal, "@");
  ASSERT_EQ(t.offset, 0);
}

TEST(Lexer, Scan_Newline_ReturnsTerminator) {
  auto f = File::from_source("test.txt", "\n");
  Lexer l;
  l.init(f.get());
  auto t = l.scan();

  ASSERT_EQ(t.kind, Token::Kind::Terminator);
  ASSERT_EQ(t.literal, "\n");
  ASSERT_EQ(t.offset, 0);
}

TEST(Lexer, Scan_DoubleSlash_ReturnsComment) {
  auto f = File::from_source("test.txt", "//");
  Lexer l;
  l.init(f.get());
  auto t = l.scan();

  ASSERT_EQ(t.kind, Token::Kind::Comment);
  ASSERT_EQ(t.literal, "//");
  ASSERT_EQ(t.offset, 0);
}

// Identifier tests
TEST(Lexer, Scan_Identifier) {
  auto f = File::from_source("test.txt", "_abc_ABC_123");
  Lexer l;
  l.init(f.get());
  auto t = l.scan();

  ASSERT_EQ(t.kind, Token::Kind::Identifier);
  ASSERT_EQ(t.literal, "_abc_ABC_123");
  ASSERT_EQ(t.offset, 0);
}

TEST(Lexer, Scan_Identifier_UnderscoreOnly) {
  auto f = File::from_source("test.txt", "_");
  Lexer l;
  l.init(f.get());
  auto t = l.scan();

  ASSERT_EQ(t.kind, Token::Kind::Identifier);
  ASSERT_EQ(t.literal, "_");
  ASSERT_EQ(t.offset, 0);
}

TEST(Lexer, Scan_Identifier_SingleCharacter) {
  auto f = File::from_source("test.txt", "a");
  Lexer l;
  l.init(f.get());
  auto t = l.scan();

  ASSERT_EQ(t.kind, Token::Kind::Identifier);
  ASSERT_EQ(t.literal, "a");
  ASSERT_EQ(t.offset, 0);
}

TEST(Lexer, Scan_Identifier_WithQuestionMark) {
  Lexer l;
  auto f = File::from_source("test.txt", "active?");
  l.init(f.get());
  auto t = l.scan();

  ASSERT_EQ(t.kind, Token::Kind::Identifier);
  ASSERT_EQ(t.literal, "active?");
  ASSERT_EQ(t.offset, 0);
}

// Number literal tests
TEST(Lexer, Scan_NumberLiteral_Integer) {
  Lexer l;
  auto f = File::from_source("test.txt", "42");
  l.init(f.get());
  auto t = l.scan();

  ASSERT_EQ(t.kind, Token::Kind::IntegerLiteral);
  ASSERT_EQ(t.literal, "42");
  ASSERT_EQ(t.offset, 0);
}

TEST(Lexer, Scan_NumberLiteral_Binary) {
  Lexer l;
  auto f = File::from_source("test.txt", "0b0101");
  l.init(f.get());
  auto t = l.scan();

  ASSERT_EQ(t.kind, Token::Kind::IntegerLiteral);
  ASSERT_EQ(t.literal, "0b0101");
  ASSERT_EQ(t.offset, 0);
}

TEST(Lexer, Scan_NumberLiteral_BinaryWithUnderscore) {
  Lexer l;
  auto f = File::from_source("test.txt", "0b0000_1111");
  l.init(f.get());
  auto t = l.scan();

  ASSERT_EQ(t.kind, Token::Kind::IntegerLiteral);
  ASSERT_EQ(t.literal, "0b0000_1111");
  ASSERT_EQ(t.offset, 0);
}

TEST(Lexer, Scan_NumberLiteral_Hex) {
  Lexer l;
  auto f = File::from_source("test.txt", "0x0123456789abcdef");
  l.init(f.get());
  auto t = l.scan();

  ASSERT_EQ(t.kind, Token::Kind::IntegerLiteral);
  ASSERT_EQ(t.literal, "0x0123456789abcdef");
  ASSERT_EQ(t.offset, 0);
}

TEST(Lexer, Scan_NumberLiteral_HexWithUnderscore) {
  Lexer l;
  auto f = File::from_source("test.txt", "0xdead_beef");
  l.init(f.get());
  auto t = l.scan();

  ASSERT_EQ(t.kind, Token::Kind::IntegerLiteral);
  ASSERT_EQ(t.literal, "0xdead_beef");
  ASSERT_EQ(t.offset, 0);
}

TEST(Lexer, Scan_NumberLiteral_Octal) {
  Lexer l;
  auto f = File::from_source("test.txt", "0o1234567");
  l.init(f.get());
  auto t = l.scan();

  ASSERT_EQ(t.kind, Token::Kind::IntegerLiteral);
  ASSERT_EQ(t.literal, "0o1234567");
  ASSERT_EQ(t.offset, 0);
}

TEST(Lexer, Scan_NumberLiteral_Float) {
  Lexer l;
  auto f = File::from_source("test.txt", "3.14");
  l.init(f.get());
  auto t = l.scan();

  ASSERT_EQ(t.kind, Token::Kind::FloatLiteral);
  ASSERT_EQ(t.literal, "3.14");
  ASSERT_EQ(t.offset, 0);
}

TEST(Lexer, Scan_NumberLiteral_Float_StartingZero) {
  Lexer l;
  auto f = File::from_source("test.txt", "0.42");
  l.init(f.get());
  auto t = l.scan();

  ASSERT_EQ(t.kind, Token::Kind::FloatLiteral);
  ASSERT_EQ(t.literal, "0.42");
  ASSERT_EQ(t.offset, 0);
}

// String literal tests
TEST(Lexer, Scan_StringLiteral) {
  Lexer l;
  auto f = File::from_source("test.txt", "\"a\"");
  l.init(f.get());
  auto t = l.scan();

  ASSERT_EQ(t.kind, Token::Kind::StringLiteral);
  ASSERT_EQ(t.literal, "\"a\"");
  ASSERT_EQ(t.offset, 0);
}

TEST(Lexer, Scan_StringLiteral_MultipleCharacters) {
  Lexer l;
  auto f = File::from_source("test.txt", "\"hello\"");
  l.init(f.get());
  auto t = l.scan();

  ASSERT_EQ(t.kind, Token::Kind::StringLiteral);
  ASSERT_EQ(t.literal, "\"hello\"");
  ASSERT_EQ(t.offset, 0);
}

TEST(Lexer, Scan_StringLiteral_EscapedQuote) {
  Lexer l;
  auto f = File::from_source("test.txt", R"str("say \"hello\"")str");
  l.init(f.get());
  auto t = l.scan();

  ASSERT_EQ(t.kind, Token::Kind::StringLiteral);
  ASSERT_EQ(t.literal, R"str("say \"hello\"")str");
  ASSERT_EQ(t.offset, 0);
}

TEST(Lexer, Scan_StringLiteral_EscapedBrace) {
  // \{ must not be treated as the start of interpolation
  Lexer l;
  auto f = File::from_source("test.txt", R"("a \{ b")");
  l.init(f.get());
  auto t = l.scan();

  ASSERT_EQ(t.kind, Token::Kind::StringLiteral);
  ASSERT_EQ(t.literal, R"("a \{ b")");
  ASSERT_EQ(t.offset, 0);
}

TEST(Lexer, Scan_StringLiteral_EscapedBackslashThenQuote) {
  // \\ consumes two chars, leaving the " as the real closing quote
  Lexer l;
  auto f = File::from_source("test.txt", R"("a\\")");
  l.init(f.get());
  auto t = l.scan();

  ASSERT_EQ(t.kind, Token::Kind::StringLiteral);
  ASSERT_EQ(t.literal, R"("a\\")");
  ASSERT_EQ(t.offset, 0);
}

TEST(Lexer, Scan_StringStart) {
  Lexer l;
  auto f = File::from_source("test.txt", "\"hello {}");
  l.init(f.get());
  auto t = l.scan();

  ASSERT_EQ(t.kind, Token::Kind::StringStart);
  ASSERT_EQ(t.literal, "\"hello {");
  ASSERT_EQ(t.offset, 0);
}

TEST(Lexer, Scan_StringMiddle) {
  Lexer l;
  auto f = File::from_source("test.txt", "\"hello {} then {");
  l.init(f.get());
  auto t = l.scan(); // skip the first token
  t = l.scan();

  GTEST_LOG_(INFO) << std::format("kind: {}", static_cast<int>(t.kind));
  GTEST_LOG_(INFO) << std::format("literal: {}", t.literal);
  GTEST_LOG_(INFO) << std::format("offset: {}", t.offset);

  ASSERT_EQ(t.kind, Token::Kind::StringMiddle);
  ASSERT_EQ(t.literal, "} then {");
  ASSERT_EQ(t.offset, 8);
}

TEST(Lexer, Scan_StringEnd) {
  Lexer l;
  auto f = File::from_source("test.txt", "\"hello {}\"");
  l.init(f.get());
  l.scan(); // skip the first token
  auto t = l.scan();

  ASSERT_EQ(t.kind, Token::Kind::StringEnd);
  ASSERT_EQ(t.literal, "}\"");
  ASSERT_EQ(t.offset, 8);
}

TEST(Lexer, Scan_StringEnd_AfterStringMiddle) {
  Lexer l;
  auto f = File::from_source("test.txt", "\"hello {} then {}\"");
  l.init(f.get());
  l.scan(); // skip the first token
  l.scan(); // skip the second token
  auto t = l.scan();

  ASSERT_EQ(t.kind, Token::Kind::StringEnd);
  ASSERT_EQ(t.literal, "}\"");
  ASSERT_EQ(t.offset, 16);
}

// Encapsulation operators
TEST(Lexer, Scan_LeftBrace) {
  Lexer l;
  auto f = File::from_source("test.txt", "{");
  l.init(f.get());
  auto t = l.scan();

  ASSERT_EQ(t.kind, Token::Kind::LeftBrace);
  ASSERT_EQ(t.literal, "{");
  ASSERT_EQ(t.offset, 0);
}

TEST(Lexer, Scan_RightBrace) {
  Lexer l;
  auto f = File::from_source("test.txt", "}");
  l.init(f.get());
  auto t = l.scan();

  ASSERT_EQ(t.kind, Token::Kind::RightBrace);
  ASSERT_EQ(t.literal, "}");
  ASSERT_EQ(t.offset, 0);
}

TEST(Lexer, Scan_LeftBracket) {
  Lexer l;
  auto f = File::from_source("test.txt", "[");
  l.init(f.get());
  auto t = l.scan();

  ASSERT_EQ(t.kind, Token::Kind::LeftBracket);
  ASSERT_EQ(t.literal, "[");
  ASSERT_EQ(t.offset, 0);
}

TEST(Lexer, Scan_RightBracket) {
  Lexer l;
  auto f = File::from_source("test.txt", "]");
  l.init(f.get());
  auto t = l.scan();

  ASSERT_EQ(t.kind, Token::Kind::RightBracket);
  ASSERT_EQ(t.literal, "]");
  ASSERT_EQ(t.offset, 0);
}

TEST(Lexer, Scan_LeftParenthesis) {
  Lexer l;
  auto f = File::from_source("test.txt", "(");
  l.init(f.get());
  auto t = l.scan();

  ASSERT_EQ(t.kind, Token::Kind::LeftParenthesis);
  ASSERT_EQ(t.literal, "(");
  ASSERT_EQ(t.offset, 0);
}

TEST(Lexer, Scan_RightParenthesis) {
  Lexer l;
  auto f = File::from_source("test.txt", ")");
  l.init(f.get());
  auto t = l.scan();

  ASSERT_EQ(t.kind, Token::Kind::RightParenthesis);
  ASSERT_EQ(t.literal, ")");
  ASSERT_EQ(t.offset, 0);
}

// Punctuation
TEST(Lexer, Scan_Dot) {
  Lexer l;
  auto f = File::from_source("test.txt", ".");
  l.init(f.get());
  auto t = l.scan();

  ASSERT_EQ(t.kind, Token::Kind::Dot);
  ASSERT_EQ(t.literal, ".");
  ASSERT_EQ(t.offset, 0);
}

TEST(Lexer, Scan_DotDot) {
  Lexer l;
  auto f = File::from_source("test.txt", "..");
  l.init(f.get());
  auto t = l.scan();

  ASSERT_EQ(t.kind, Token::Kind::DotDot);
  ASSERT_EQ(t.literal, "..");
  ASSERT_EQ(t.offset, 0);
}

TEST(Lexer, Scan_Ellipsis) {
  Lexer l;
  auto f = File::from_source("test.txt", "...");
  l.init(f.get());
  auto t = l.scan();

  ASSERT_EQ(t.kind, Token::Kind::Ellipsis);
  ASSERT_EQ(t.literal, "...");
  ASSERT_EQ(t.offset, 0);
}

TEST(Lexer, Scan_Colon) {
  Lexer l;
  auto f = File::from_source("test.txt", ":");
  l.init(f.get());
  auto t = l.scan();

  ASSERT_EQ(t.kind, Token::Kind::Colon);
  ASSERT_EQ(t.literal, ":");
  ASSERT_EQ(t.offset, 0);
}

TEST(Lexer, Scan_Comma) {
  Lexer l;
  auto f = File::from_source("test.txt", ",");
  l.init(f.get());
  auto t = l.scan();

  ASSERT_EQ(t.kind, Token::Kind::Comma);
  ASSERT_EQ(t.literal, ",");
  ASSERT_EQ(t.offset, 0);
}

TEST(Lexer, Scan_Semicolon) {
  Lexer l;
  auto f = File::from_source("test.txt", ";");
  l.init(f.get());
  auto t = l.scan();

  ASSERT_EQ(t.kind, Token::Kind::Semicolon);
  ASSERT_EQ(t.literal, ";");
  ASSERT_EQ(t.offset, 0);
}

TEST(Lexer, Scan_QuestionMark) {
  Lexer l;
  auto f = File::from_source("test.txt", "?");
  l.init(f.get());
  auto t = l.scan();

  ASSERT_EQ(t.kind, Token::Kind::QuestionMark);
  ASSERT_EQ(t.literal, "?");
  ASSERT_EQ(t.offset, 0);
}

// Assignment operators
TEST(Lexer, Scan_Assignment) {
  Lexer l;
  auto f = File::from_source("test.txt", "=");
  l.init(f.get());
  auto t = l.scan();

  ASSERT_EQ(t.kind, Token::Kind::Assignment);
  ASSERT_EQ(t.literal, "=");
  ASSERT_EQ(t.offset, 0);
}

TEST(Lexer, Scan_DeclAssignment) {
  Lexer l;
  auto f = File::from_source("test.txt", ":=");
  l.init(f.get());
  auto t = l.scan();

  ASSERT_EQ(t.kind, Token::Kind::DeclAssignment);
  ASSERT_EQ(t.literal, ":=");
  ASSERT_EQ(t.offset, 0);
}

// Mathematical operators
TEST(Lexer, Scan_Add) {
  Lexer l;
  auto f = File::from_source("test.txt", "+");
  l.init(f.get());
  auto t = l.scan();

  ASSERT_EQ(t.kind, Token::Kind::Add);
  ASSERT_EQ(t.literal, "+");
  ASSERT_EQ(t.offset, 0);
}

TEST(Lexer, Scan_Increment) {
  Lexer l;
  auto f = File::from_source("test.txt", "++");
  l.init(f.get());
  auto t = l.scan();

  ASSERT_EQ(t.kind, Token::Kind::Increment);
  ASSERT_EQ(t.literal, "++");
  ASSERT_EQ(t.offset, 0);
}

TEST(Lexer, Scan_Sub) {
  Lexer l;
  auto f = File::from_source("test.txt", "-");
  l.init(f.get());
  auto t = l.scan();

  ASSERT_EQ(t.kind, Token::Kind::Sub);
  ASSERT_EQ(t.literal, "-");
  ASSERT_EQ(t.offset, 0);
}

TEST(Lexer, Scan_Decrement) {
  Lexer l;
  auto f = File::from_source("test.txt", "--");
  l.init(f.get());
  auto t = l.scan();

  ASSERT_EQ(t.kind, Token::Kind::Decrement);
  ASSERT_EQ(t.literal, "--");
  ASSERT_EQ(t.offset, 0);
}

TEST(Lexer, Scan_Multiply) {
  Lexer l;
  auto f = File::from_source("test.txt", "*");
  l.init(f.get());
  auto t = l.scan();

  ASSERT_EQ(t.kind, Token::Kind::Multiply);
  ASSERT_EQ(t.literal, "*");
  ASSERT_EQ(t.offset, 0);
}

TEST(Lexer, Scan_Divide) {
  Lexer l;
  auto f = File::from_source("test.txt", "/");
  l.init(f.get());
  auto t = l.scan();

  ASSERT_EQ(t.kind, Token::Kind::Divide);
  ASSERT_EQ(t.literal, "/");
  ASSERT_EQ(t.offset, 0);
}

TEST(Lexer, Scan_Modulo) {
  Lexer l;
  auto f = File::from_source("test.txt", "%");
  l.init(f.get());
  auto t = l.scan();

  ASSERT_EQ(t.kind, Token::Kind::Modulo);
  ASSERT_EQ(t.literal, "%");
  ASSERT_EQ(t.offset, 0);
}

// Logical operators
TEST(Lexer, Scan_LogicalOr) {
  Lexer l;
  auto f = File::from_source("test.txt", "||");
  l.init(f.get());
  auto t = l.scan();

  ASSERT_EQ(t.kind, Token::Kind::LogicalOr);
  ASSERT_EQ(t.literal, "||");
  ASSERT_EQ(t.offset, 0);
}

TEST(Lexer, Scan_LogicalAnd) {
  Lexer l;
  auto f = File::from_source("test.txt", "&&");
  l.init(f.get());
  auto t = l.scan();

  ASSERT_EQ(t.kind, Token::Kind::LogicalAnd);
  ASSERT_EQ(t.literal, "&&");
  ASSERT_EQ(t.offset, 0);
}

TEST(Lexer, Scan_Equal) {
  Lexer l;
  auto f = File::from_source("test.txt", "==");
  l.init(f.get());
  auto t = l.scan();

  ASSERT_EQ(t.kind, Token::Kind::Equal);
  ASSERT_EQ(t.literal, "==");
  ASSERT_EQ(t.offset, 0);
}

TEST(Lexer, Scan_Not) {
  Lexer l;
  auto f = File::from_source("test.txt", "!");
  l.init(f.get());
  auto t = l.scan();

  ASSERT_EQ(t.kind, Token::Kind::Not);
  ASSERT_EQ(t.literal, "!");
  ASSERT_EQ(t.offset, 0);
}

TEST(Lexer, Scan_NotEqual) {
  Lexer l;
  auto f = File::from_source("test.txt", "!=");
  l.init(f.get());
  auto t = l.scan();

  ASSERT_EQ(t.kind, Token::Kind::NotEqual);
  ASSERT_EQ(t.literal, "!=");
  ASSERT_EQ(t.offset, 0);
}

TEST(Lexer, Scan_LessThan) {
  Lexer l;
  auto f = File::from_source("test.txt", "<");
  l.init(f.get());
  auto t = l.scan();

  ASSERT_EQ(t.kind, Token::Kind::LessThan);
  ASSERT_EQ(t.literal, "<");
  ASSERT_EQ(t.offset, 0);
}

TEST(Lexer, Scan_LessThanEqual) {
  Lexer l;
  auto f = File::from_source("test.txt", "<=");
  l.init(f.get());
  auto t = l.scan();

  ASSERT_EQ(t.kind, Token::Kind::LessThanEqual);
  ASSERT_EQ(t.literal, "<=");
  ASSERT_EQ(t.offset, 0);
}

TEST(Lexer, Scan_GreaterThan) {
  Lexer l;
  auto f = File::from_source("test.txt", ">");
  l.init(f.get());
  auto t = l.scan();

  ASSERT_EQ(t.kind, Token::Kind::GreaterThan);
  ASSERT_EQ(t.literal, ">");
  ASSERT_EQ(t.offset, 0);
}

TEST(Lexer, Scan_GreaterThanEqual) {
  Lexer l;
  auto f = File::from_source("test.txt", ">=");
  l.init(f.get());
  auto t = l.scan();

  ASSERT_EQ(t.kind, Token::Kind::GreaterThanEqual);
  ASSERT_EQ(t.literal, ">=");
  ASSERT_EQ(t.offset, 0);
}

// Bitwise operators
TEST(Lexer, Scan_LeftShift) {
  Lexer l;
  auto f = File::from_source("test.txt", "<<");
  l.init(f.get());
  auto t = l.scan();

  ASSERT_EQ(t.kind, Token::Kind::LeftShift);
  ASSERT_EQ(t.literal, "<<");
  ASSERT_EQ(t.offset, 0);
}

TEST(Lexer, Scan_RightShift) {
  Lexer l;
  auto f = File::from_source("test.txt", ">>");
  l.init(f.get());
  auto t = l.scan();

  ASSERT_EQ(t.kind, Token::Kind::RightShift);
  ASSERT_EQ(t.literal, ">>");
  ASSERT_EQ(t.offset, 0);
}

TEST(Lexer, Scan_BitwiseAnd) {
  Lexer l;
  auto f = File::from_source("test.txt", "&");
  l.init(f.get());
  auto t = l.scan();

  ASSERT_EQ(t.kind, Token::Kind::BitwiseAnd);
  ASSERT_EQ(t.literal, "&");
  ASSERT_EQ(t.offset, 0);
}

TEST(Lexer, Scan_BitwiseOr) {
  Lexer l;
  auto f = File::from_source("test.txt", "|");
  l.init(f.get());
  auto t = l.scan();

  ASSERT_EQ(t.kind, Token::Kind::BitwiseOr);
  ASSERT_EQ(t.literal, "|");
  ASSERT_EQ(t.offset, 0);
}

TEST(Lexer, Scan_BitwiseNot) {
  Lexer l;
  auto f = File::from_source("test.txt", "~");
  l.init(f.get());
  auto t = l.scan();

  ASSERT_EQ(t.kind, Token::Kind::BitwiseNot);
  ASSERT_EQ(t.literal, "~");
  ASSERT_EQ(t.offset, 0);
}

TEST(Lexer, Scan_BitwiseXor) {
  Lexer l;
  auto f = File::from_source("test.txt", "^");
  l.init(f.get());
  auto t = l.scan();

  ASSERT_EQ(t.kind, Token::Kind::BitwiseXor);
  ASSERT_EQ(t.literal, "^");
  ASSERT_EQ(t.offset, 0);
}

// Keyword tests
TEST(Lexer, Scan_BreakKeyword) {
  Lexer l;
  auto f = File::from_source("test.txt", "break");
  l.init(f.get());
  auto t = l.scan();

  ASSERT_EQ(t.kind, Token::Kind::Break);
  ASSERT_EQ(t.literal, "break");
  ASSERT_EQ(t.offset, 0);
}

TEST(Lexer, Scan_ConstKeyword) {
  Lexer l;
  auto f = File::from_source("test.txt", "const");
  l.init(f.get());
  auto t = l.scan();

  ASSERT_EQ(t.kind, Token::Kind::Const);
  ASSERT_EQ(t.literal, "const");
  ASSERT_EQ(t.offset, 0);
}

TEST(Lexer, Scan_ElseKeyword) {
  Lexer l;
  auto f = File::from_source("test.txt", "else");
  l.init(f.get());
  auto t = l.scan();

  ASSERT_EQ(t.kind, Token::Kind::Else);
  ASSERT_EQ(t.literal, "else");
  ASSERT_EQ(t.offset, 0);
}

TEST(Lexer, Scan_EnumKeyword) {
  Lexer l;
  auto f = File::from_source("test.txt", "enum");
  l.init(f.get());
  auto t = l.scan();

  ASSERT_EQ(t.kind, Token::Kind::Enum);
  ASSERT_EQ(t.literal, "enum");
  ASSERT_EQ(t.offset, 0);
}

TEST(Lexer, Scan_FalseKeyword) {
  Lexer l;
  auto f = File::from_source("test.txt", "false");
  l.init(f.get());
  auto t = l.scan();

  ASSERT_EQ(t.kind, Token::Kind::BoolLiteral);
  ASSERT_EQ(t.literal, "false");
  ASSERT_EQ(t.offset, 0);
}

TEST(Lexer, Scan_FnKeyword) {
  Lexer l;
  auto f = File::from_source("test.txt", "fn");
  l.init(f.get());
  auto t = l.scan();

  ASSERT_EQ(t.kind, Token::Kind::Fn);
  ASSERT_EQ(t.literal, "fn");
  ASSERT_EQ(t.offset, 0);
}

TEST(Lexer, Scan_ForKeyword) {
  Lexer l;
  auto f = File::from_source("test.txt", "for");
  l.init(f.get());
  auto t = l.scan();

  ASSERT_EQ(t.kind, Token::Kind::For);
  ASSERT_EQ(t.literal, "for");
  ASSERT_EQ(t.offset, 0);
}

TEST(Lexer, Scan_IfKeyword) {
  Lexer l;
  auto f = File::from_source("test.txt", "if");
  l.init(f.get());
  auto t = l.scan();

  ASSERT_EQ(t.kind, Token::Kind::If);
  ASSERT_EQ(t.literal, "if");
  ASSERT_EQ(t.offset, 0);
}

TEST(Lexer, Scan_ImportKeyword) {
  Lexer l;
  auto f = File::from_source("test.txt", "import");
  l.init(f.get());
  auto t = l.scan();

  ASSERT_EQ(t.kind, Token::Kind::Import);
  ASSERT_EQ(t.literal, "import");
  ASSERT_EQ(t.offset, 0);
}

TEST(Lexer, Scan_InterfaceKeyword) {
  Lexer l;
  auto f = File::from_source("test.txt", "interface");
  l.init(f.get());
  auto t = l.scan();

  ASSERT_EQ(t.kind, Token::Kind::Interface);
  ASSERT_EQ(t.literal, "interface");
  ASSERT_EQ(t.offset, 0);
}

TEST(Lexer, Scan_NextKeyword) {
  Lexer l;
  auto f = File::from_source("test.txt", "next");
  l.init(f.get());
  auto t = l.scan();

  ASSERT_EQ(t.kind, Token::Kind::Next);
  ASSERT_EQ(t.literal, "next");
  ASSERT_EQ(t.offset, 0);
}

TEST(Lexer, Scan_OrKeyword) {
  Lexer l;
  auto f = File::from_source("test.txt", "or");
  l.init(f.get());
  auto t = l.scan();

  ASSERT_EQ(t.kind, Token::Kind::Or);
  ASSERT_EQ(t.literal, "or");
  ASSERT_EQ(t.offset, 0);
}

TEST(Lexer, Scan_PubKeyword) {
  Lexer l;
  auto f = File::from_source("test.txt", "pub");
  l.init(f.get());
  auto t = l.scan();

  ASSERT_EQ(t.kind, Token::Kind::Pub);
  ASSERT_EQ(t.literal, "pub");
  ASSERT_EQ(t.offset, 0);
}

TEST(Lexer, Scan_ReturnKeyword) {
  Lexer l;
  auto f = File::from_source("test.txt", "return");
  l.init(f.get());
  auto t = l.scan();

  ASSERT_EQ(t.kind, Token::Kind::Return);
  ASSERT_EQ(t.literal, "return");
  ASSERT_EQ(t.offset, 0);
}

TEST(Lexer, Scan_SpawnKeyword) {
  Lexer l;
  auto f = File::from_source("test.txt", "spawn");
  l.init(f.get());
  auto t = l.scan();

  ASSERT_EQ(t.kind, Token::Kind::Spawn);
  ASSERT_EQ(t.literal, "spawn");
  ASSERT_EQ(t.offset, 0);
}

TEST(Lexer, Scan_StructKeyword) {
  Lexer l;
  auto f = File::from_source("test.txt", "struct");
  l.init(f.get());
  auto t = l.scan();

  ASSERT_EQ(t.kind, Token::Kind::Struct);
  ASSERT_EQ(t.literal, "struct");
  ASSERT_EQ(t.offset, 0);
}

TEST(Lexer, Scan_SwitchKeyword) {
  Lexer l;
  auto f = File::from_source("test.txt", "switch");
  l.init(f.get());
  auto t = l.scan();

  ASSERT_EQ(t.kind, Token::Kind::Switch);
  ASSERT_EQ(t.literal, "switch");
  ASSERT_EQ(t.offset, 0);
}

TEST(Lexer, Scan_TrueKeyword) {
  Lexer l;
  auto f = File::from_source("test.txt", "true");
  l.init(f.get());
  auto t = l.scan();

  ASSERT_EQ(t.kind, Token::Kind::BoolLiteral);
  ASSERT_EQ(t.literal, "true");
  ASSERT_EQ(t.offset, 0);
}

// Multi-token tests
TEST(Lexer, Scan_Multiple) {
  Lexer l;
  auto f = File::from_source("test.txt", "a := 42");
  l.init(f.get());

  // First Token
  auto t = l.scan();

  ASSERT_EQ(t.kind, Token::Kind::Identifier);
  ASSERT_EQ(t.literal, "a");
  ASSERT_EQ(t.offset, 0);

  // Second Token
  t = l.scan();

  ASSERT_EQ(t.kind, Token::Kind::DeclAssignment);
  ASSERT_EQ(t.literal, ":=");
  ASSERT_EQ(t.offset, 2);

  // Third Token
  t = l.scan();

  ASSERT_EQ(t.kind, Token::Kind::IntegerLiteral);
  ASSERT_EQ(t.literal, "42");
  ASSERT_EQ(t.offset, 5);
}

// Newline position tracking
TEST(Lexer, Scan_Newline_TracksPosition) {
  auto f = File::from_source("test.txt", "a\nb");
  Lexer l;
  l.init(f.get());

  l.scan();          // 'a' — Identifier on line 1
  l.scan();          // '\n' — Terminator, registers line 2
  auto t = l.scan(); // 'b' — Identifier on line 2

  ASSERT_EQ(t.kind, Token::Kind::Identifier);
  ASSERT_EQ(t.literal, "b");
  ASSERT_EQ(t.offset, 2);

  auto pos = f->position_at(2);
  ASSERT_EQ(pos.line, 2);
  ASSERT_EQ(pos.column, 1);
}

} // namespace mc

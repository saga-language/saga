#include "frontend/lexer.hpp"
#include "frontend/token.hpp"

#include <gtest/gtest.h>

namespace mc {

// Empty source and special characters
TEST(Lexer, Scan_EmptySource_ReturnsEof) {
  auto l = Lexer{};
  l.file = File::from_source("test.txt", "");
  l.error_list = ErrorList{};
  auto t = l.scan();

  ASSERT_EQ(t.kind, Token::Kind::Eof);
  ASSERT_EQ(t.literal, "");
  ASSERT_EQ(t.offset, 0);
}

TEST(Lexer, Scan_InvalidCharacter_ReturnsInvalid) {
  auto l = Lexer{};
  l.file = File::from_source("test.txt", "@");
  l.error_list = ErrorList{};
  auto t = l.scan();

  ASSERT_EQ(l.error_list.errors.size(), 1);

  ASSERT_EQ(t.kind, Token::Kind::Invalid);
  ASSERT_EQ(t.literal, "@");
  ASSERT_EQ(t.offset, 0);
}

TEST(Lexer, Scan_Newline_ReturnsTerminator) {
  auto l = Lexer{};
  l.file = File::from_source("test.txt", "\n");
  l.error_list = ErrorList{};
  auto t = l.scan();

  ASSERT_EQ(t.kind, Token::Kind::Terminator);
  ASSERT_EQ(t.literal, "\n");
  ASSERT_EQ(t.offset, 0);
}

TEST(Lexer, Scan_DoubleSlash_ReturnsComment) {
  auto l = Lexer{};
  l.file = File::from_source("test.txt", "//");
  l.error_list = ErrorList{};
  auto t = l.scan();

  ASSERT_EQ(t.kind, Token::Kind::Comment);
  ASSERT_EQ(t.literal, "//");
  ASSERT_EQ(t.offset, 0);
}

// Identifier tests
TEST(Lexer, Scan_Identifier) {
  auto l = Lexer{};
  l.file = File::from_source("test.txt", "_abc_ABC_123");
  l.error_list = ErrorList{};
  auto t = l.scan();

  ASSERT_EQ(t.kind, Token::Kind::Identifier);
  ASSERT_EQ(t.literal, "_abc_ABC_123");
  ASSERT_EQ(t.offset, 0);
}

TEST(Lexer, Scan_Identifier_UnderscoreOnly) {
  auto l = Lexer{};
  l.file = File::from_source("test.txt", "_");
  l.error_list = ErrorList{};
  auto t = l.scan();

  ASSERT_EQ(t.kind, Token::Kind::Identifier);
  ASSERT_EQ(t.literal, "_");
  ASSERT_EQ(t.offset, 0);
}

TEST(Lexer, Scan_Identifier_SingleCharacter) {
  auto l = Lexer{};
  l.file = File::from_source("test.txt", "a");
  l.error_list = ErrorList{};
  auto t = l.scan();

  ASSERT_EQ(t.kind, Token::Kind::Identifier);
  ASSERT_EQ(t.literal, "a");
  ASSERT_EQ(t.offset, 0);
}

// Number literal tests
TEST(Lexer, Scan_NumberLiteral_Integer) {
  auto l = Lexer{};
  l.file = File::from_source("test.txt", "42");
  l.error_list = ErrorList{};
  auto t = l.scan();

  ASSERT_EQ(t.kind, Token::Kind::Number);
  ASSERT_EQ(t.literal, "42");
  ASSERT_EQ(t.offset, 0);
}

TEST(Lexer, Scan_NumberLiteral_Binary) {
  auto l = Lexer{};
  l.file = File::from_source("test.txt", "0b0101");
  l.error_list = ErrorList{};
  auto t = l.scan();

  ASSERT_EQ(t.kind, Token::Kind::Number);
  ASSERT_EQ(t.literal, "0b0101");
  ASSERT_EQ(t.offset, 0);
}

TEST(Lexer, Scan_NumberLiteral_BinaryWithUnderscore) {
  auto l = Lexer{};
  l.file = File::from_source("test.txt", "0b0000_1111");
  l.error_list = ErrorList{};
  auto t = l.scan();

  ASSERT_EQ(t.kind, Token::Kind::Number);
  ASSERT_EQ(t.literal, "0b0000_1111");
  ASSERT_EQ(t.offset, 0);
}

TEST(Lexer, Scan_NumberLiteral_Hex) {
  auto l = Lexer{};
  l.file = File::from_source("test.txt", "0x1f");
  l.error_list = ErrorList{};
  auto t = l.scan();

  ASSERT_EQ(t.kind, Token::Kind::Number);
  ASSERT_EQ(t.literal, "0x1f");
  ASSERT_EQ(t.offset, 0);
}

TEST(Lexer, Scan_NumberLiteral_HexWithUnderscore) {
  auto l = Lexer{};
  l.file = File::from_source("test.txt", "0xdead_beef");
  l.error_list = ErrorList{};
  auto t = l.scan();

  ASSERT_EQ(t.kind, Token::Kind::Number);
  ASSERT_EQ(t.literal, "0xdead_beef");
  ASSERT_EQ(t.offset, 0);
}

TEST(Lexer, Scan_NumberLiteral_Octal) {
  auto l = Lexer{};
  l.file = File::from_source("test.txt", "0o1234567");
  l.error_list = ErrorList{};
  auto t = l.scan();

  ASSERT_EQ(t.kind, Token::Kind::Number);
  ASSERT_EQ(t.literal, "0o1234567");
  ASSERT_EQ(t.offset, 0);
}

TEST(Lexer, Scan_NumberLiteral_Float) {
  auto l = Lexer{};
  l.file = File::from_source("test.txt", "3.14");
  l.error_list = ErrorList{};
  auto t = l.scan();

  ASSERT_EQ(t.kind, Token::Kind::Number);
  ASSERT_EQ(t.literal, "3.14");
  ASSERT_EQ(t.offset, 0);
}

TEST(Lexer, Scan_NumberLiteral_Float_StartingZero) {
  auto l = Lexer{};
  l.file = File::from_source("test.txt", "0.42");
  l.error_list = ErrorList{};
  auto t = l.scan();

  ASSERT_EQ(t.kind, Token::Kind::Number);
  ASSERT_EQ(t.literal, "0.42");
  ASSERT_EQ(t.offset, 0);
}

// String literal tests
TEST(Lexer, Scan_StringLiteral) {
  auto l = Lexer{};
  l.file = File::from_source("test.txt", "\"a\"");
  l.error_list = ErrorList{};
  auto t = l.scan();

  ASSERT_EQ(t.kind, Token::Kind::String);
  ASSERT_EQ(t.literal, "\"a\"");
  ASSERT_EQ(t.offset, 0);
}

TEST(Lexer, Scan_StringLiteral_MultipleCharacters) {
  auto l = Lexer{};
  l.file = File::from_source("test.txt", "\"hello\"");
  l.error_list = ErrorList{};
  auto t = l.scan();

  ASSERT_EQ(t.kind, Token::Kind::String);
  ASSERT_EQ(t.literal, "\"hello\"");
  ASSERT_EQ(t.offset, 0);
}

// Encapsulation operators
TEST(Lexer, Scan_LeftBrace) {
  auto l = Lexer{};
  l.file = File::from_source("test.txt", "{");
  l.error_list = ErrorList{};
  auto t = l.scan();

  ASSERT_EQ(t.kind, Token::Kind::LeftBrace);
  ASSERT_EQ(t.literal, "{");
  ASSERT_EQ(t.offset, 0);
}

TEST(Lexer, Scan_RightBrace) {
  auto l = Lexer{};
  l.file = File::from_source("test.txt", "}");
  l.error_list = ErrorList{};
  auto t = l.scan();

  ASSERT_EQ(t.kind, Token::Kind::RightBrace);
  ASSERT_EQ(t.literal, "}");
  ASSERT_EQ(t.offset, 0);
}

TEST(Lexer, Scan_LeftBracket) {
  auto l = Lexer{};
  l.file = File::from_source("test.txt", "[");
  l.error_list = ErrorList{};
  auto t = l.scan();

  ASSERT_EQ(t.kind, Token::Kind::LeftBracket);
  ASSERT_EQ(t.literal, "[");
  ASSERT_EQ(t.offset, 0);
}

TEST(Lexer, Scan_RightBracket) {
  auto l = Lexer{};
  l.file = File::from_source("test.txt", "]");
  l.error_list = ErrorList{};
  auto t = l.scan();

  ASSERT_EQ(t.kind, Token::Kind::RightBracket);
  ASSERT_EQ(t.literal, "]");
  ASSERT_EQ(t.offset, 0);
}

TEST(Lexer, Scan_LeftParenthesis) {
  auto l = Lexer{};
  l.file = File::from_source("test.txt", "(");
  l.error_list = ErrorList{};
  auto t = l.scan();

  ASSERT_EQ(t.kind, Token::Kind::LeftParenthesis);
  ASSERT_EQ(t.literal, "(");
  ASSERT_EQ(t.offset, 0);
}

TEST(Lexer, Scan_RightParenthesis) {
  auto l = Lexer{};
  l.file = File::from_source("test.txt", ")");
  l.error_list = ErrorList{};
  auto t = l.scan();

  ASSERT_EQ(t.kind, Token::Kind::RightParenthesis);
  ASSERT_EQ(t.literal, ")");
  ASSERT_EQ(t.offset, 0);
}

// Assignment and Dot operators
TEST(Lexer, Scan_Assignment) {
  auto l = Lexer{};
  l.file = File::from_source("test.txt", ":=");
  l.error_list = ErrorList{};
  auto t = l.scan();

  ASSERT_EQ(t.kind, Token::Kind::Assignment);
  ASSERT_EQ(t.literal, ":=");
  ASSERT_EQ(t.offset, 0);
}

TEST(Lexer, Scan_Dot) {
  auto l = Lexer{};
  l.file = File::from_source("test.txt", ".");
  l.error_list = ErrorList{};
  auto t = l.scan();

  ASSERT_EQ(t.kind, Token::Kind::Dot);
  ASSERT_EQ(t.literal, ".");
  ASSERT_EQ(t.offset, 0);
}

// Mathematical operators
TEST(Lexer, Scan_Plus) {
  auto l = Lexer{};
  l.file = File::from_source("test.txt", "+");
  l.error_list = ErrorList{};
  auto t = l.scan();

  ASSERT_EQ(t.kind, Token::Kind::Plus);
  ASSERT_EQ(t.literal, "+");
  ASSERT_EQ(t.offset, 0);
}

TEST(Lexer, Scan_Minus) {
  auto l = Lexer{};
  l.file = File::from_source("test.txt", "-");
  l.error_list = ErrorList{};
  auto t = l.scan();

  ASSERT_EQ(t.kind, Token::Kind::Minus);
  ASSERT_EQ(t.literal, "-");
  ASSERT_EQ(t.offset, 0);
}

TEST(Lexer, Scan_Multiply) {
  auto l = Lexer{};
  l.file = File::from_source("test.txt", "*");
  l.error_list = ErrorList{};
  auto t = l.scan();

  ASSERT_EQ(t.kind, Token::Kind::Multiply);
  ASSERT_EQ(t.literal, "*");
  ASSERT_EQ(t.offset, 0);
}

TEST(Lexer, Scan_Divide) {
  auto l = Lexer{};
  l.file = File::from_source("test.txt", "/");
  l.error_list = ErrorList{};
  auto t = l.scan();

  ASSERT_EQ(t.kind, Token::Kind::Divide);
  ASSERT_EQ(t.literal, "/");
  ASSERT_EQ(t.offset, 0);
}

TEST(Lexer, Scan_Modulo) {
  auto l = Lexer{};
  l.file = File::from_source("test.txt", "%");
  l.error_list = ErrorList{};
  auto t = l.scan();

  ASSERT_EQ(t.kind, Token::Kind::Modulo);
  ASSERT_EQ(t.literal, "%");
  ASSERT_EQ(t.offset, 0);
}

// Logical operators
TEST(Lexer, Scan_LogicalOr) {
  auto l = Lexer{};
  l.file = File::from_source("test.txt", "||");
  l.error_list = ErrorList{};
  auto t = l.scan();

  ASSERT_EQ(t.kind, Token::Kind::LogicalOr);
  ASSERT_EQ(t.literal, "||");
  ASSERT_EQ(t.offset, 0);
}

TEST(Lexer, Scan_LogicalAnd) {
  auto l = Lexer{};
  l.file = File::from_source("test.txt", "&&");
  l.error_list = ErrorList{};
  auto t = l.scan();

  ASSERT_EQ(t.kind, Token::Kind::LogicalAnd);
  ASSERT_EQ(t.literal, "&&");
  ASSERT_EQ(t.offset, 0);
}

TEST(Lexer, Scan_Equal) {
  auto l = Lexer{};
  l.file = File::from_source("test.txt", "=");
  l.error_list = ErrorList{};
  auto t = l.scan();

  ASSERT_EQ(t.kind, Token::Kind::Equal);
  ASSERT_EQ(t.literal, "=");
  ASSERT_EQ(t.offset, 0);
}

TEST(Lexer, Scan_NotEqual) {
  auto l = Lexer{};
  l.file = File::from_source("test.txt", "!=");
  l.error_list = ErrorList{};
  auto t = l.scan();

  ASSERT_EQ(t.kind, Token::Kind::NotEqual);
  ASSERT_EQ(t.literal, "!=");
  ASSERT_EQ(t.offset, 0);
}

TEST(Lexer, Scan_LessThan) {
  auto l = Lexer{};
  l.file = File::from_source("test.txt", "<");
  l.error_list = ErrorList{};
  auto t = l.scan();

  ASSERT_EQ(t.kind, Token::Kind::LessThan);
  ASSERT_EQ(t.literal, "<");
  ASSERT_EQ(t.offset, 0);
}

TEST(Lexer, Scan_LessThanEqual) {
  auto l = Lexer{};
  l.file = File::from_source("test.txt", "<=");
  l.error_list = ErrorList{};
  auto t = l.scan();

  ASSERT_EQ(t.kind, Token::Kind::LessThanEqual);
  ASSERT_EQ(t.literal, "<=");
  ASSERT_EQ(t.offset, 0);
}

TEST(Lexer, Scan_GreaterThan) {
  auto l = Lexer{};
  l.file = File::from_source("test.txt", ">");
  l.error_list = ErrorList{};
  auto t = l.scan();

  ASSERT_EQ(t.kind, Token::Kind::GreaterThan);
  ASSERT_EQ(t.literal, ">");
  ASSERT_EQ(t.offset, 0);
}

TEST(Lexer, Scan_GreaterThanEqual) {
  auto l = Lexer{};
  l.file = File::from_source("test.txt", ">=");
  l.error_list = ErrorList{};
  auto t = l.scan();

  ASSERT_EQ(t.kind, Token::Kind::GreaterThanEqual);
  ASSERT_EQ(t.literal, ">=");
  ASSERT_EQ(t.offset, 0);
}

// Bitwise operators
TEST(Lexer, Scan_LeftShift) {
  auto l = Lexer{};
  l.file = File::from_source("test.txt", "<<");
  l.error_list = ErrorList{};
  auto t = l.scan();

  ASSERT_EQ(t.kind, Token::Kind::LeftShift);
  ASSERT_EQ(t.literal, "<<");
  ASSERT_EQ(t.offset, 0);
}

TEST(Lexer, Scan_RightShift) {
  auto l = Lexer{};
  l.file = File::from_source("test.txt", ">>");
  l.error_list = ErrorList{};
  auto t = l.scan();

  ASSERT_EQ(t.kind, Token::Kind::RightShift);
  ASSERT_EQ(t.literal, ">>");
  ASSERT_EQ(t.offset, 0);
}

TEST(Lexer, Scan_BitwiseAnd) {
  auto l = Lexer{};
  l.file = File::from_source("test.txt", "&");
  l.error_list = ErrorList{};
  auto t = l.scan();

  ASSERT_EQ(t.kind, Token::Kind::BitwiseAnd);
  ASSERT_EQ(t.literal, "&");
  ASSERT_EQ(t.offset, 0);
}

TEST(Lexer, Scan_BitwiseOr) {
  auto l = Lexer{};
  l.file = File::from_source("test.txt", "|");
  l.error_list = ErrorList{};
  auto t = l.scan();

  ASSERT_EQ(t.kind, Token::Kind::BitwiseOr);
  ASSERT_EQ(t.literal, "|");
  ASSERT_EQ(t.offset, 0);
}

TEST(Lexer, Scan_BitwiseNot) {
  auto l = Lexer{};
  l.file = File::from_source("test.txt", "!");
  l.error_list = ErrorList{};
  auto t = l.scan();

  ASSERT_EQ(t.kind, Token::Kind::BitwiseNot);
  ASSERT_EQ(t.literal, "!");
  ASSERT_EQ(t.offset, 0);
}

TEST(Lexer, Scan_BitwiseXor) {
  auto l = Lexer{};
  l.file = File::from_source("test.txt", "^");
  l.error_list = ErrorList{};
  auto t = l.scan();

  ASSERT_EQ(t.kind, Token::Kind::BitwiseXor);
  ASSERT_EQ(t.literal, "^");
  ASSERT_EQ(t.offset, 0);
}

// Keyword tests
TEST(Lexer, Scan_BreakKeyword) {
  auto l = Lexer{};
  l.file = File::from_source("test.txt", "break");
  l.error_list = ErrorList{};
  auto t = l.scan();

  ASSERT_EQ(t.kind, Token::Kind::Else);
  ASSERT_EQ(t.literal, "break");
  ASSERT_EQ(t.offset, 0);
}

TEST(Lexer, Scan_ElseKeyword) {
  auto l = Lexer{};
  l.file = File::from_source("test.txt", "else");
  l.error_list = ErrorList{};
  auto t = l.scan();

  ASSERT_EQ(t.kind, Token::Kind::Else);
  ASSERT_EQ(t.literal, "else");
  ASSERT_EQ(t.offset, 0);
}

TEST(Lexer, Scan_ForKeyword) {
  auto l = Lexer{};
  l.file = File::from_source("test.txt", "for");
  l.error_list = ErrorList{};
  auto t = l.scan();

  ASSERT_EQ(t.kind, Token::Kind::For);
  ASSERT_EQ(t.literal, "for");
  ASSERT_EQ(t.offset, 0);
}

TEST(Lexer, Scan_IfKeyword) {
  auto l = Lexer{};
  l.file = File::from_source("test.txt", "if");
  l.error_list = ErrorList{};
  auto t = l.scan();

  ASSERT_EQ(t.kind, Token::Kind::If);
  ASSERT_EQ(t.literal, "if");
  ASSERT_EQ(t.offset, 0);
}

TEST(Lexer, Scan_ImportKeyword) {
  auto l = Lexer{};
  l.file = File::from_source("test.txt", "import");
  l.error_list = ErrorList{};
  auto t = l.scan();

  ASSERT_EQ(t.kind, Token::Kind::Import);
  ASSERT_EQ(t.literal, "import");
  ASSERT_EQ(t.offset, 0);
}

TEST(Lexer, Scan_NextKeyword) {
  auto l = Lexer{};
  l.file = File::from_source("test.txt", "next");
  l.error_list = ErrorList{};
  auto t = l.scan();

  ASSERT_EQ(t.kind, Token::Kind::Next);
  ASSERT_EQ(t.literal, "next");
  ASSERT_EQ(t.offset, 0);
}

TEST(Lexer, Scan_OrKeyword) {
  auto l = Lexer{};
  l.file = File::from_source("test.txt", "or");
  l.error_list = ErrorList{};
  auto t = l.scan();

  ASSERT_EQ(t.kind, Token::Kind::Or);
  ASSERT_EQ(t.literal, "or");
  ASSERT_EQ(t.offset, 0);
}

TEST(Lexer, Scan_ReturnKeyword) {
  auto l = Lexer{};
  l.file = File::from_source("test.txt", "return");
  l.error_list = ErrorList{};
  auto t = l.scan();

  ASSERT_EQ(t.kind, Token::Kind::Return);
  ASSERT_EQ(t.literal, "return");
  ASSERT_EQ(t.offset, 0);
}

TEST(Lexer, Scan_SpawnKeyword) {
  auto l = Lexer{};
  l.file = File::from_source("test.txt", "spawn");
  l.error_list = ErrorList{};
  auto t = l.scan();

  ASSERT_EQ(t.kind, Token::Kind::Spawn);
  ASSERT_EQ(t.literal, "spawn");
  ASSERT_EQ(t.offset, 0);
}

TEST(Lexer, Scan_StructKeyword) {
  auto l = Lexer{};
  l.file = File::from_source("test.txt", "struct");
  l.error_list = ErrorList{};
  auto t = l.scan();

  ASSERT_EQ(t.kind, Token::Kind::Struct);
  ASSERT_EQ(t.literal, "struct");
  ASSERT_EQ(t.offset, 0);
}

// Multi-token tests
TEST(Lexer, Scan_Multiple) {
  auto l = Lexer{};
  l.file = File::from_source("test.txt", "a := 42");
  l.error_list = ErrorList{};

  // First Token
  auto t = l.scan();

  ASSERT_EQ(t.kind, Token::Kind::Identifier);
  ASSERT_EQ(t.literal, "a");
  ASSERT_EQ(t.offset, 0);

  // Second Token
  t = l.scan();

  ASSERT_EQ(t.kind, Token::Kind::Assignment);
  ASSERT_EQ(t.literal, ":=");
  ASSERT_EQ(t.offset, 2);

  // Third Token
  t = l.scan();

  ASSERT_EQ(t.kind, Token::Kind::Number);
  ASSERT_EQ(t.literal, "42");
  ASSERT_EQ(t.offset, 5);
}

} // namespace mc

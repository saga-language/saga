#pragma once

#include <string_view>

namespace mc {
struct Token {
  enum class Kind {
    // Special characters
    Eof,
    Invalid,
    Terminator, // '\n'
    Comment,    // "//"
    BackSlash,  // '\'
    // Literals
    Identifier,   // _abcABC123
    Number,       // 42, 1_000, 0b1010, 0x1a3f, 0o755, 123,45, 0.5, 123.45e10
    String,       // "a", "hello"
    StringStart,  // "Hello {
    StringMiddle, // }, I'm {
    StringEnd,    // } years old."
    Void,         // "void"
    // Keywords
    Break,     // "break"
    Else,      // "else"
    For,       // "for"
    If,        // "if"
    Import,    // "import"
    Interface, // "interface"
    Next,      // "next"
    Or,        // "or"
    Pub,       // "pub"
    Return,    // "return"
    Spawn,     // "spawn"
    Struct,    // "struct"
    Switch,    // "switch"
    // Operators
    Assignment, // ':='
    Comma,      // ','
    Colon,      // ':'
    Dot,        // '.'
    // Mathematical Operators
    Plus,     // '+'
    Minus,    // '-'
    Multiply, // '*'
    Divide,   // '/'
    Modulo,   // '%'
    // Encapsulation,
    LeftBrace,        // '{'
    RightBrace,       // '}'
    LeftBracket,      // '['
    RightBracket,     // ']'
    LeftParenthesis,  // '('
    RightParenthesis, // ')'
    // Logical Operators
    LogicalOr,        // '||'
    LogicalAnd,       // '&&'
    Equal,            // '='
    NotEqual,         // '!='
    LessThan,         // '<'
    LessThanEqual,    // '<='
    GreaterThan,      // '>'
    GreaterThanEqual, // '>='
    // Bitwise Operators
    LeftShift,  // '>>'
    RightShift, // '<<'
    BitwiseAnd, // '&'
    BitwiseOr,  // '|'
    BitwiseNot, // '!'
    BitwiseXor, // '^'
  };

  Kind kind;
  std::string_view literal;
  size_t offset;
};
} // namespace mc

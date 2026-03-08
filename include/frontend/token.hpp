#pragma once

#include <string_view>

namespace mc {
struct Token {
  enum class Kind {
    // Special characters
    Eof,        // End of File
    Invalid,    // Bad/Unknown character
    Terminator, // '\n'
    Comment,    // "//"

    // Literals
    Identifier,     // _, _abcABC123, boolean?
    BoolLiteral,    // true, false
    FloatLiteral,   // 1_123,45, 0.5, 123.4_5e+1_001
    IntegerLiteral, // 42, 1_000, 0b1010, 0x1a3f, 0o755
    StringLiteral,  // "a", "hello", """multi-line string"""

    // Keywords
    Break,     // "break"
    Case,      // "case"
    Const,     // "const"
    Else,      // "else"
    Enum,      // "enum"
    Fn,        // "fn"
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

    // Punctuation
    Comma,        // ','
    Colon,        // ':'
    Dot,          // '.'
    DotDot,       // '..'
    Ellipsis,     // '...'
    QuestionMark, // '?'
    Semicolon,    // ';'

    // Assignment Operators
    Assignment,     // '='
    AddAssignment,  // '+='
    DeclAssignment, // ':='
    SubAssignment,  // '-='
    MulAssignment,  // '*='
    DivAssignment,  // '/='

    // Arithmetic Operators
    Add,       // '+'
    Decrement, // '--'
    Divide,    // '/'
    Increment, // '++'
    Modulo,    // '%'
    Multiply,  // '*'
    Pow,       // '**'
    Sub,       // '-'

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
    Equal,            // '=='
    Not,              // '!'
    NotEqual,         // '!='
    LessThan,         // '<'
    LessThanEqual,    // '<='
    GreaterThan,      // '>'
    GreaterThanEqual, // '>='

    // Bitwise Operators
    BitwiseAnd, // '&'
    BitwiseNot, // '~'
    BitwiseOr,  // '|'
    BitwiseXor, // '^'
    LeftShift,  // '<<'
    RightShift, // '>>'
  };

  Kind kind;
  std::string_view literal;
  size_t offset;
};
} // namespace mc

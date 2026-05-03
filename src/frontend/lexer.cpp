// Copywrite 2026 Rob Thornton
// SPDX-License-Identifier: MIT

#include "frontend/lexer.hpp"
#include "frontend/token.hpp"

#include <string_view>
#include <unordered_map>

namespace saga {

inline static const std::unordered_map<std::string_view, Token::Kind> keywords =
    {{"break", Token::Kind::Break},
     {"case", Token::Kind::Case},
     {"const", Token::Kind::Const},
     {"else", Token::Kind::Else},
     {"enum", Token::Kind::Enum},
     {"false", Token::Kind::BoolLiteral},
     {"fn", Token::Kind::Fn},
     {"for", Token::Kind::For},
     {"if", Token::Kind::If},
     {"import", Token::Kind::Import},
     {"interface", Token::Kind::Interface},
     {"next", Token::Kind::Next},
     {"or", Token::Kind::Or},
     {"pub", Token::Kind::Pub},
     {"return", Token::Kind::Return},
     {"spawn", Token::Kind::Spawn},
     {"struct", Token::Kind::Struct},
     {"switch", Token::Kind::Switch},
     {"true", Token::Kind::BoolLiteral}};

void Lexer::init(File *file) {
  this->file = file;
  this->source = file->source;
  this->state = {};
  this->offset = 0;
  this->reading_offset = 0;
}

Token Lexer::scan() {
  skip_whitespace();

  char c = next();
  if (is_digit(c)) {
    return scan_number(c);
  }

  if (is_alpha(c)) {
    return scan_identifier();
  }

  Token::Kind t;
  switch (c) {
  case '\n':
    file->add_file_newline(reading_offset);
    return accept(Token::Kind::Terminator);
  case '"':
    if (peek() == '"' && reading_offset + 1 < source.length() &&
        source[reading_offset + 1] == '"') {
      next(); // consume second "
      next(); // consume third "
      return scan_multi_line_string(c);
    }
    return scan_string(c);
  case '/':
    if (match('/')) {
      return scan_comment();
    }
    t = match('=') ? Token::Kind::DivAssignment : Token::Kind::Divide;
    return accept(t);

  // One or Two character tokens
  case '&':
    t = match('&') ? Token::Kind::LogicalAnd : Token::Kind::BitwiseAnd;
    return accept(t);
  case '|':
    t = match('|') ? Token::Kind::LogicalOr : Token::Kind::BitwiseOr;
    return accept(t);
  case '!':
    t = match('=') ? Token::Kind::NotEqual : Token::Kind::Not;
    return accept(t);
  case '<':
    if (match('=')) {
      return accept(Token::Kind::LessThanEqual);
    }
    t = match('<') ? Token::Kind::LeftShift : Token::Kind::LessThan;
    return accept(t);
  case '>':
    if (match('=')) {
      return accept(Token::Kind::GreaterThanEqual);
    }
    t = match('>') ? Token::Kind::RightShift : Token::Kind::GreaterThan;
    return accept(t);
  case ':':
    t = match('=') ? Token::Kind::DeclAssignment : Token::Kind::Colon;
    return accept(t);
  case '*':
    if (match('*')) {
      return accept(Token::Kind::Pow);
    }
    t = match('=') ? Token::Kind::MulAssignment : Token::Kind::Multiply;
    return accept(t);
  case '+':
    if (match('+'))
      return accept(Token::Kind::Increment);
    t = match('=') ? Token::Kind::AddAssignment : Token::Kind::Add;
    return accept(t);
  case '-':
    if (match('-'))
      return accept(Token::Kind::Decrement);
    t = match('=') ? Token::Kind::SubAssignment : Token::Kind::Sub;
    return accept(t);

    // Single character tokens
  case '.':
    if (match('.')) {
      t = match('.') ? Token::Kind::Ellipsis : Token::Kind::DotDot;
      return accept(t);
    }
    return accept(Token::Kind::Dot);
  case ',':
    return accept(Token::Kind::Comma);
  case '%':
    return accept(Token::Kind::Modulo);
  case '(':
    return accept(Token::Kind::LeftParenthesis);
  case ')':
    return accept(Token::Kind::RightParenthesis);
  case '{':
    return accept(Token::Kind::LeftBrace);
  case '}':
    if (is_interpolating_multi_line()) {
      return scan_multi_line_string(c);
    }
    if (is_interpolating()) {
      return scan_string(c);
    }
    return accept(Token::Kind::RightBrace);
  case '[':
    return accept(Token::Kind::LeftBracket);
  case ']':
    return accept(Token::Kind::RightBracket);
  case '=':
    t = match('=') ? Token::Kind::Equal : Token::Kind::Assignment;
    return accept(t);
  case '^':
    return accept(Token::Kind::BitwiseXor);
  case '~':
    return accept(Token::Kind::BitwiseNot);
  case ';':
    return accept(Token::Kind::Semicolon);
  case '?':
    return accept(Token::Kind::QuestionMark);
  default:
    if (c != 0) {
      error_list.report_error(file->position_at(reading_offset),
                              "Unexpected token");
      return accept(Token::Kind::Invalid);
    }
  }

  return accept(Token::Kind::Eof);
}

Token Lexer::accept(Token::Kind kind) {
  auto literal = source.substr(offset, reading_offset - offset);
  auto token = Token{kind, literal, offset};

  offset = reading_offset;

  return token;
}

bool Lexer::match(const char c) {
  if (peek() == c) {
    next();
    return true;
  }
  return false;
}

char Lexer::next() {
  char c = peek();
  if (c != 0) {
    reading_offset++;
  }
  return c;
}

char Lexer::peek() {
  if (is_eof()) {
    return 0;
  }
  return source[reading_offset]; // do not advance
}

bool Lexer::is_alpha(const char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}
bool Lexer::is_binary(const char c) { return c == '0' || c == '1' || c == '_'; }
bool Lexer::is_digit(const char c) { return c >= '0' && c <= '9'; }
bool Lexer::is_eof() { return reading_offset >= source.length(); }
bool Lexer::is_hex(const char c) {
  return is_digit(c) || ((c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')) ||
         c == '_';
}
bool Lexer::is_interpolating() {
  return !state.empty() && (state.back() == LexerState::InString ||
                            state.back() == LexerState::InMultiLineString);
}
bool Lexer::is_interpolating_multi_line() {
  return !state.empty() && state.back() == LexerState::InMultiLineString;
}
bool Lexer::is_octal(const char c) { return c >= '0' && c <= '7'; }
bool Lexer::is_whitespace(char c) { return c == ' ' || c == '\t' || c == '\r'; }

Token Lexer::scan_binary() {
  while (is_binary(peek())) {
    next();
  }
  return accept(Token::Kind::IntegerLiteral);
}

Token Lexer::scan_comment() {
  while (peek() != '\n' && !is_eof()) {
    next();
  }
  return accept(Token::Kind::Comment);
}

// TODO: basic float, should handle scientific notation
Token Lexer::scan_float() {
  while ((is_digit(peek()) || peek() == '_') && !is_eof()) {
    next();
  }

  return accept(Token::Kind::FloatLiteral);
}

Token Lexer::scan_hex() {
  while (is_hex(peek()) && !is_eof()) {
    next();
  }
  return accept(Token::Kind::IntegerLiteral);
}

Token Lexer::scan_identifier() {
  while (is_alpha(peek()) || is_digit(peek())) {
    next();
  }

  // allow trailing question marks in identifiers
  if (peek() == '?') {
    next();
  }

  auto literal = source.substr(offset, reading_offset - offset);
  auto kind = kind_for_alphanumeric(literal);
  return accept(kind);
}

Token Lexer::scan_number(const char c) {
  if (c == '0') {
    switch (peek()) {
    case 'b':
      next();
      return scan_binary();
    case 'o':
      next();
      return scan_octal();
    case 'x':
      next();
      return scan_hex();
    }
  }

  while (is_digit(peek()) || peek() == '_') {
    next();
  }

  if (peek() == '.' && !(reading_offset + 1 < source.length() && source[reading_offset + 1] == '.')) {
    next();
    return scan_float();
  }

  return accept(Token::Kind::IntegerLiteral);
}

Token Lexer::scan_octal() {
  while (is_octal(peek())) {
    next();
  }
  return accept(Token::Kind::IntegerLiteral);
}

Token Lexer::scan_multi_line_string(const char c) {
  Token::Kind mode = Token::Kind::StringLiteral;
  if (c == '}') {
    mode = Token::Kind::StringEnd;
    if (is_interpolating()) {
      state.pop_back();
    }
  }

  while (true) {
    if (is_eof()) {
      error_list.report_error(file->position_at(reading_offset),
                              "Unexpected EOF in multi-line string.");
      return accept(Token::Kind::Invalid);
    }

    // Check for closing """
    if (peek() == '"' && reading_offset + 2 < source.length() &&
        source[reading_offset + 1] == '"' &&
        source[reading_offset + 2] == '"') {
      next(); // consume first "
      next(); // consume second "
      next(); // consume third "
      return accept(mode);
    }

    if (peek() == '\\') {
      next(); // consume the backslash
      next(); // skip whatever follows — don't interpret it
      continue;
    }

    if (peek() == '{') {
      // start of interpolated expression; stop scanning this fragment
      if (mode == Token::Kind::StringEnd) {
        mode = Token::Kind::StringMiddle;
      } else {
        mode = Token::Kind::StringStart;
      }
      state.push_back(LexerState::InMultiLineString);
      break;
    }

    if (peek() == '\n') {
      file->add_file_newline(reading_offset + 1);
    }

    next();
  }

  next(); // consume the '{' that triggered the break
  return accept(mode);
}

Token Lexer::scan_string(const char c) {
  Token::Kind mode = Token::Kind::StringLiteral;
  if (c == '}') {
    mode = Token::Kind::StringEnd;
    if (is_interpolating()) {
      state.pop_back();
    }
  }

  while (peek() != '"') {
    if (is_eof()) {
      error_list.report_error(file->position_at(reading_offset),
                              "Unexpected EOF.");
      return accept(Token::Kind::Invalid);
    }

    if (peek() == '\\') {
      next(); // consume the backslash
      next(); // skip whatever follows — don't interpret it
      continue;
    }

    if (peek() == '{') {
      // start of interpolated expression; stop scanning this fragment
      if (mode == Token::Kind::StringEnd) {
        mode = Token::Kind::StringMiddle;
      } else {
        mode = Token::Kind::StringStart;
      }
      state.push_back(LexerState::InString);
      break;
    }

    next();
  }

  next();
  return accept(mode);
}

void Lexer::skip_whitespace() {
  while (is_whitespace(peek())) {
    next();
  }
  offset = reading_offset;
}

Token::Kind Lexer::kind_for_alphanumeric(std::string_view literal) {
  auto it = keywords.find(literal);
  if (it != keywords.end()) {
    return it->second;
  }

  return Token::Kind::Identifier;
}
} // namespace saga

#include "frontend/lexer.hpp"
#include "frontend/token.hpp"

#include <string_view>
#include <unordered_map>

namespace mc {

inline static const std::unordered_map<std::string_view, Token::Kind> keywords =
    {{"break", Token::Kind::Break},   {"else", Token::Kind::Else},
     {"for", Token::Kind::For},       {"if", Token::Kind::If},
     {"import", Token::Kind::Import}, {"interface", Token::Kind::Interface},
     {"next", Token::Kind::Next},     {"or", Token::Kind::Or},
     {"pub", Token::Kind::Pub},       {"return", Token::Kind::Return},
     {"spawn", Token::Kind::Spawn},   {"struct", Token::Kind::Struct},
     {"switch", Token::Kind::Switch}, {"void", Token::Kind::Void}};

void Lexer::init(File *file) {
  this->file = file;
  this->source = file->source;
  this->state = {Token::Kind::Void};
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
    return accept(Token::Kind::Terminator);
  case '"':
    return scan_string(c);
  case '/':
    if (match('/')) {
      return scan_comment();
    }
    return accept(Token::Kind::Divide);

  // One or Two character tokens
  case '&':
    t = match('&') ? Token::Kind::LogicalAnd : Token::Kind::BitwiseAnd;
    return accept(t);
  case '|':
    t = match('|') ? Token::Kind::LogicalOr : Token::Kind::BitwiseOr;
    return accept(t);
  case '!':
    t = match('=') ? Token::Kind::NotEqual : Token::Kind::BitwiseNot;
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

  // Two chracter tokens
  case ':':
    if (match('=')) {
      return accept(Token::Kind::Assignment);
    }
    return accept(Token::Kind::Colon);

    // Single character tokens
  case '.':
    return accept(Token::Kind::Dot);
  case ',':
    return accept(Token::Kind::Comma);
  case '*':
    return accept(Token::Kind::Multiply);
  case '+':
    return accept(Token::Kind::Plus);
  case '-':
    return accept(Token::Kind::Minus);
  case '%':
    return accept(Token::Kind::Modulo);
  case '(':
    return accept(Token::Kind::LeftParenthesis);
  case ')':
    return accept(Token::Kind::RightParenthesis);
  case '{':
    return accept(Token::Kind::LeftBrace);
  case '}':
    if (is_interpolating()) {
      return scan_string(c);
    }
    return accept(Token::Kind::RightBrace);
  case '[':
    return accept(Token::Kind::LeftBracket);
  case ']':
    return accept(Token::Kind::RightBracket);
  case '=':
    return accept(Token::Kind::Equal);
  case '^':
    return accept(Token::Kind::BitwiseXor);
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
bool Lexer::is_escaped() { return state.back() == Token::Kind::BackSlash; }
bool Lexer::is_hex(const char c) {
  return is_digit(c) || ((c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')) ||
         c == '_';
}
bool Lexer::is_interpolating() {
  return state.back() == Token::Kind::StringStart ||
         state.back() == Token::Kind::StringMiddle;
}
bool Lexer::is_octal(const char c) { return c >= '0' && c <= '7'; }
bool Lexer::is_whitespace(char c) { return c == ' ' || c == '\t' || c == '\r'; }

Token Lexer::scan_binary() {
  while (is_binary(peek())) {
    next();
  }
  return accept(Token::Kind::Number);
}

Token Lexer::scan_comment() {
  while (peek() != '\n' && !is_eof()) {
    next();
  }
  next();
  return accept(Token::Kind::Comment);
}

// TODO: basic float, should handle scientific notation
Token Lexer::scan_float() {
  while ((is_digit(peek()) || peek() == '_') && !is_eof()) {
    next();
  }

  return accept(Token::Kind::Number);
}

Token Lexer::scan_hex() {
  while (is_hex(peek()) && !is_eof()) {
    next();
  }
  return accept(Token::Kind::Number);
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

  if (peek() == '.') {
    next();
    return scan_float();
  }

  return accept(Token::Kind::Number);
}

Token Lexer::scan_octal() {
  while (is_octal(peek())) {
    next();
  }
  return accept(Token::Kind::Number);
}

Token Lexer::scan_string(const char c) {
  Token::Kind mode = Token::Kind::String;
  if (c == '}' && !is_escaped()) {
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
      state.push_back(Token::Kind::BackSlash);
    }

    if (peek() == '{') {
      if (is_escaped()) {
        state.pop_back(); // consume the escape
      } else {
        // start of interpolated string; stop scanning
        if (mode == Token::Kind::StringEnd) {
          mode = Token::Kind::StringMiddle;
        } else {
          mode = Token::Kind::StringStart;
        }
        state.push_back(mode);
        break;
      }
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
} // namespace mc

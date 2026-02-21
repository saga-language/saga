#pragma once

#include "error_list.hpp"
#include "file.hpp"
#include "token.hpp"

namespace mc {
struct Lexer {
  File file;
  ErrorList error_list;
  size_t offset = 0;
  size_t reading_offset = 0;

  Token scan();

private:
  Token accept(Token::Kind k);
  Token expect(const char c, const Token::Kind k);

  bool match(const char c);

  char next();
  char peek();

  bool is_alpha(const char c);
  bool is_binary(const char c);
  bool is_digit(const char c);
  bool is_eof();
  bool is_hex(const char c);
  bool is_octal(const char c);
  bool is_whitespace(char c);

  Token scan_binary();
  Token scan_comment();
  Token scan_float();
  Token scan_hex();
  Token scan_identifier();
  Token scan_number(char c);
  Token scan_octal();
  Token scan_string();

  void skip_whitespace();

  Token::Kind kind_for_alphanumeric(std::string_view literal);
};
} // namespace mc

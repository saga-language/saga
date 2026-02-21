#pragma once

#include "ast.hpp"
#include "token.hpp"
#include <vector>

namespace mc {
struct Parser {
  const std::vector<Token> &tokens;
  size_t pos = 0;

  NodePtr parse_program();

private:
  NodePtr parse_expression();
};
} // namespace mc

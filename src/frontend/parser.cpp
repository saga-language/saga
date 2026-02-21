#include "frontend/parser.hpp"
#include "frontend/ast.hpp"

mc::NodePtr mc::Parser::parse_program() {
  Token t = Token{Token::Kind::Number, "42", 0};
  auto n = make_node<IntegerLiteral>(t);
  std::vector<NodePtr> expressions;

  expressions.push_back(n);

  return make_node<Program>(expressions);
}

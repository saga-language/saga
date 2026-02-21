#pragma once

#include "span.hpp"
#include "token.hpp"

#include <charconv>
#include <memory>
#include <stdexcept>
#include <utility>
#include <variant>
#include <vector>

namespace mc {
struct Node;

using NodePtr = std::shared_ptr<const Node>;

struct IntegerLiteral {
  Token token;

  int value() const { // TODO: probably should be in the CPP file.
    int value;
    auto start = token.literal.data();
    auto end = token.literal.data() + token.literal.size();
    auto [ptr, ec] = std::from_chars(start, end, value);
    if (ec != std::errc()) {
      throw std::runtime_error("Invalid integer literal: " +
                               std::string(token.literal));
    }

    return value;
  }

  Span span() const {
    return {token.offset, token.offset + token.literal.size()};
  }
};

// program entrypoint
struct Program {
  std::vector<NodePtr> expressions;
};

struct Node {
  using Variant = std::variant<IntegerLiteral, Program>;

  Variant node;

  template <typename T> Node(T value) : node(std::move(value)) {}
};

template <typename T, typename... Args> NodePtr make_node(Args &&...args) {
  return std::make_shared<const Node>(T{std::forward<Args>(args)...});
}
} // namespace mc

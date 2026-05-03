// Copywrite 2026 Rob Thornton
// SPDX-License-Identifier: MIT

#pragma once

#include <format>
#include <string_view>

namespace saga {
struct Position {
  std::string_view filename;
  size_t line;
  size_t column;
};
} // namespace saga

template <> struct std::formatter<saga::Position> {
  constexpr auto parse(std::format_parse_context &ctx) { return ctx.begin(); }

  auto format(const saga::Position &pos, std::format_context &ctx) const {
    return std::format_to(ctx.out(), "{}:{}:{}", pos.filename, pos.line,
                          pos.column);
  }
};

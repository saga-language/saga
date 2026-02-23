#pragma once

#include <format>
#include <string_view>

namespace mc {
struct Position {
  std::string_view filename;
  size_t line;
  size_t column;
};
} // namespace mc

template <> struct std::formatter<mc::Position> {
  constexpr auto parse(std::format_parse_context &ctx) { return ctx.begin(); }

  auto format(const mc::Position &pos, std::format_context &ctx) const {
    return std::format_to(ctx.out(), "{}:{}:{}", pos.filename, pos.line,
                          pos.column);
  }
};

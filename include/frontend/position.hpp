// Copywrite 2026 Rob Thornton
// SPDX-License-Identifier: MIT

#pragma once

#include <format>
#include <string>

namespace saga {
// Owns its file name: a diagnostic can outlive the file set that placed it, as
// an imported package's errors outlive the sub-analysis that found them.
struct Position {
  std::string filename;
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

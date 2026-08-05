// Copywrite 2026 Rob Thornton
// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace saga {

// One raw string-fragment token: its source text, delimiters included, and the
// absolute offset of its first character.
struct RawFragment {
  std::string_view text;
  size_t offset;
};

struct BlockStringError {
  size_t offset;
  std::string message;
};

// `margin` is the width of the indentation the closing `"""` sits at, and every
// content line carries. It is engaged only for a well-formed block string: an
// inline `"""..."""`, a plain `"..."`, and any literal that errored all leave it
// empty, which tells the unescaper to treat the text as content verbatim.
struct BlockStringLayout {
  std::optional<size_t> margin;
  std::vector<BlockStringError> errors;
};

// Resolve the layout of one string literal from its fragments in source order.
BlockStringLayout block_string_layout(std::span<const RawFragment> fragments);

} // namespace saga

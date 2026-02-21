#pragma once

#include "position.hpp"

#include <string>
#include <vector>

namespace mc {
struct File {
  std::string_view name;
  std::string_view source;
  size_t base_offset = 0;
  std::vector<size_t> line_offsets;

  size_t end_offset() const { return base_offset + source.length(); }
  const Position position_at(std::size_t offset) const;

  static File from_path(const std::string &path);
  static File from_source(std::string path, std::string source);
};
} // namespace mc

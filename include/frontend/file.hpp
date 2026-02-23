#pragma once

#include "position.hpp"

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace mc {
struct File {
  std::string filename;
  std::string source;
  std::vector<size_t> line_offsets;

  File(std::string filename, std::string source)
      : filename(std::move(filename)), source(std::move(source)) {
    line_offsets.push_back(0); // Always a line starting at offset zero
  }

  void add_file_newline(size_t offset);
  Position position_at(std::size_t offset) const;

  static std::unique_ptr<File> from_source(std::string filename,
                                           std::string source);
};
} // namespace mc

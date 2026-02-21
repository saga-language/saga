#pragma once

#include "file.hpp"
#include "position.hpp"

#include <vector>

namespace mc {
struct FileSet {
  std::vector<File> files;

  void add_file(File file);
  const Position position_at(std::size_t offset) const;

private:
  const File &find_file(size_t offset) const;
};
} // namespace mc

#include "frontend/fileset.hpp"
#include "frontend/file.hpp"

#include <algorithm>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <vector>

namespace mc {

void FileSet::add_file(File file) {
  std::size_t base =
      files.empty() ? 0 : files.back().base_offset + files.back().source.size();

  file.base_offset = base;
  files.push_back(std::move(file));
}

const mc::Position mc::FileSet::position_at(std::size_t offset) const {
  const File &file = find_file(offset);

  return file.position_at(offset);
}

const mc::File &mc::FileSet::find_file(size_t offset) const {
  // Use binary search over linear search for potentially large filesets
  auto it = std::upper_bound(files.begin(), files.end(), offset,
                             [](std::size_t value, const File &file) {
                               return value < file.base_offset;
                             });

  if (it == files.begin()) {
    throw std::runtime_error("Offset before first file");
  }

  --it;

  if (offset >= it->base_offset && offset < it->end_offset()) {
    return *it;
  }

  throw std::runtime_error("No file found in set for offset " +
                           std::to_string(offset));
}

} // namespace mc

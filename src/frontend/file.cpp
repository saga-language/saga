#include "frontend/file.hpp"

#include <algorithm>
#include <cstddef>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace mc {
namespace {
std::vector<size_t> line_offsets_from_source(const std::string &source) {
  std::vector<std::size_t> line_offsets;
  line_offsets.push_back(0);

  for (std::size_t i = 0; i < source.size(); ++i) {
    if (source[i] == '\n') {
      line_offsets.push_back(i + 1);
    }
  }

  return line_offsets;
}
} // namespace

const Position File::position_at(std::size_t offset) const {
  std::size_t local = offset - base_offset;

  auto it = std::upper_bound(line_offsets.begin(), line_offsets.end(), local);

  std::size_t line = std::distance(line_offsets.begin(), it);
  std::size_t column = local - line_offsets[line - 1];

  return {name, line, column};
}

File File::from_path(const std::string &path) {
  std::ifstream in(path);
  if (!in) {
    throw std::runtime_error("Could not open file: " + path);
  }

  std::ostringstream ss;
  ss << in.rdbuf();
  std::string source = ss.str();

  return from_source(std::move(path), std::move(source));
}

File File::from_source(std::string path, std::string source) {
  auto line_offsets = line_offsets_from_source(source);

  return File{std::move(path), std::move(source), 0, std::move(line_offsets)};
}
} // namespace mc

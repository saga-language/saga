// Copywrite 2026 Rob Thornton
// SPDX-License-Identifier: MIT

#include "frontend/file.hpp"

#include <algorithm>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

namespace saga {
void File::add_file_newline(size_t offset) { line_offsets.push_back(offset); }

Position File::position_at(std::size_t offset) const {
  auto it = std::upper_bound(line_offsets.begin(), line_offsets.end(), offset);

  std::size_t line = std::distance(line_offsets.begin(), it);
  std::size_t column = (offset - line_offsets[line - 1]) + 1;

  return {filename, line, column};
}

std::unique_ptr<File> File::from_source(std::string filename,
                                        std::string source) {
  return std::make_unique<File>(std::move(filename), std::move(source));
}
std::unique_ptr<File> File::from_path(const std::string &path) {
  std::ifstream in(path);
  if (!in.is_open())
    return nullptr;
  std::ostringstream buf;
  buf << in.rdbuf();
  return from_source(path, buf.str());
}
} // namespace saga

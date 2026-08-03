// Copywrite 2026 Rob Thornton
// SPDX-License-Identifier: MIT

#include "frontend/fileset.hpp"
#include "frontend/file.hpp"

#include <memory>
#include <vector>

namespace saga {

void FileSet::add_file(std::unique_ptr<File> file) {
  if (!files.empty()) {
    const File &prev = *files.back();
    // One past the end, so an offset at a file's EOF still belongs to it.
    file->base = prev.base + prev.source.size() + 1;
  }
  files.push_back(std::move(file));
}

const File *FileSet::file_at(size_t offset) const {
  for (auto it = files.rbegin(); it != files.rend(); ++it) {
    if (offset >= (*it)->base)
      return it->get();
  }
  return files.empty() ? nullptr : files.front().get();
}

Position FileSet::position_at(size_t offset) const {
  const File *file = file_at(offset);
  return file ? file->position_at(offset - file->base) : Position{};
}

} // namespace saga

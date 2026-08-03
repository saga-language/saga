// Copywrite 2026 Rob Thornton
// SPDX-License-Identifier: MIT

#pragma once

#include "file.hpp"
#include "position.hpp"

#include <memory>
#include <vector>

namespace saga {
// Files are laid end to end in one offset space, so a Span identifies both a
// location and the file that owns it without carrying a file id.
struct FileSet {
  std::vector<std::unique_ptr<File>> files;

  void add_file(std::unique_ptr<File> file);

  /// The file owning a FileSet offset, or nullptr if the set is empty.
  const File *file_at(size_t offset) const;

  /// Resolve a FileSet offset to a source position.
  Position position_at(size_t offset) const;
};
} // namespace saga

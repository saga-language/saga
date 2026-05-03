// Copywrite 2026 Rob Thornton
// SPDX-License-Identifier: MIT

#pragma once

#include "file.hpp"

#include <memory>
#include <vector>

namespace saga {
struct FileSet {
  std::vector<std::unique_ptr<File>> files;

  void add_file(std::unique_ptr<File> file);
};
} // namespace saga

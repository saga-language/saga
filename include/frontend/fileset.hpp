#pragma once

#include "file.hpp"

#include <memory>
#include <vector>

namespace mc {
struct FileSet {
  std::vector<std::unique_ptr<File>> files;

  void add_file(std::unique_ptr<File> file);
};
} // namespace mc

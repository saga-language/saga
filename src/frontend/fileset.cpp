#include "frontend/fileset.hpp"
#include "frontend/file.hpp"

#include <memory>
#include <vector>

namespace mc {

void FileSet::add_file(std::unique_ptr<File> file) {
  files.push_back(std::move(file));
}

} // namespace mc

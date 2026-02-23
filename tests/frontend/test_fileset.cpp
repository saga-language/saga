#include "frontend/file.hpp"
#include "frontend/fileset.hpp"

#include <gtest/gtest.h>

namespace mc {

TEST(FileSetTest, AddFile_AppendsFile) {
  FileSet fs;
  auto f = File::from_source("file.txt", "src");
  fs.add_file(std::move(f));

  ASSERT_EQ(fs.files.size(), 1);
  ASSERT_EQ(fs.files[0]->filename, "file.txt");
}

} // namespace mc

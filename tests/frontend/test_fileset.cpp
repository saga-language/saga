#include "frontend/file.hpp"
#include "frontend/fileset.hpp"

#include <gtest/gtest.h>

namespace mc {
class FileSetTest : public ::testing::Test {
protected:
  mc::FileSet fs;

  void SetUp() override { fs = {}; };
};

TEST_F(FileSetTest, AddFile_AppendsFile) {
  auto f = File::from_source("a.txt", "src");
  fs.add_file(std::move(f));

  ASSERT_EQ(fs.files.size(), 1);
  ASSERT_EQ(fs.files[0].base_offset, 0);
}

TEST_F(FileSetTest, AddFile_AppliesBaseOffset) {
  auto a = File::from_source("a.txt", "src");
  auto b = File::from_source("b.txt", "");

  fs.add_file(std::move(a));
  fs.add_file(std::move(b));

  ASSERT_EQ(fs.files.size(), 2);
  ASSERT_EQ(fs.files[0].base_offset, 0);
  ASSERT_EQ(fs.files[1].base_offset, 3);
}
} // namespace mc

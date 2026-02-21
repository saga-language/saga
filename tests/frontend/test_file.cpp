#include "frontend/file.hpp"

#include <gtest/gtest.h>

namespace mc {
TEST(File, EndOffset_Correct_ForSource) {
  File f{"filename", "src", 0, std::vector<size_t>{0}};
  EXPECT_EQ(f.end_offset(), 3);
}

TEST(File, FromSource_CreatesFile) {
  File f = File::from_source("a.txt", "src");

  ASSERT_EQ(f.name, "a.txt");
  ASSERT_EQ(f.source, "src");
  ASSERT_EQ(f.base_offset, 0);
}

TEST(File, FromPath_CreatesFile) {
  // Untested
}

} // namespace mc

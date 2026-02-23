#include "frontend/file.hpp"
#include "frontend/position.hpp"

#include <gtest/gtest.h>
#include <memory>

namespace mc {
TEST(File, FromSource_CreatesFile) {
  std::unique_ptr<File> f = File::from_source("file.txt", "src");

  GTEST_ASSERT_EQ(f->filename, "file.txt");
  GTEST_ASSERT_EQ(f->source, "src");
}

TEST(File, HasDefaultLineOffset_OfZero) {
  std::unique_ptr<File> f = File::from_source("", "");

  GTEST_ASSERT_EQ(f->line_offsets[0], 0);
}

TEST(File, AddFileNewline_AppendsOffset) {
  std::unique_ptr<File> f = File::from_source("", "");
  f->add_file_newline(42);

  GTEST_ASSERT_EQ(f->line_offsets.size(), 2);
  GTEST_ASSERT_EQ(f->line_offsets[1], 42);
}

TEST(File, PositionAt_OffsetZero) {
  std::unique_ptr<File> f = File::from_source("test.txt", "src");
  Position p = f->position_at(0);

  GTEST_ASSERT_EQ(p.filename, "test.txt");
  GTEST_ASSERT_EQ(p.column, 1);
  GTEST_ASSERT_EQ(p.line, 1);
}

TEST(File, PositionAt_SingleLine_OffsetTen) {
  std::unique_ptr<File> f = File::from_source("test.txt", "src");
  Position p = f->position_at(9);

  GTEST_ASSERT_EQ(p.filename, "test.txt");
  GTEST_ASSERT_EQ(p.column, 10);
  GTEST_ASSERT_EQ(p.line, 1);
}

TEST(File, PositionAt_MultiLine_OffsetOne) {
  std::unique_ptr<File> f = File::from_source("test.txt", "src");
  f->add_file_newline(4);

  Position p = f->position_at(1);

  GTEST_ASSERT_EQ(p.filename, "test.txt");
  GTEST_ASSERT_EQ(p.column, 2);
  GTEST_ASSERT_EQ(p.line, 1);
}

TEST(File, PositionAt_MultiLine_OffsetTen) {
  std::unique_ptr<File> f = File::from_source("test.txt", "src");
  f->add_file_newline(5);

  Position p = f->position_at(9);

  GTEST_ASSERT_EQ(p.filename, "test.txt");
  GTEST_ASSERT_EQ(p.column, 5);
  GTEST_ASSERT_EQ(p.line, 2);
}

} // namespace mc

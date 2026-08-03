// Copywrite 2026 Rob Thornton
// SPDX-License-Identifier: MIT

#include "frontend/file.hpp"
#include "frontend/fileset.hpp"

#include <gtest/gtest.h>

namespace saga {

TEST(FileSetTest, AddFile_AppendsFile) {
  FileSet fs;
  auto f = File::from_source("file.txt", "src");
  fs.add_file(std::move(f));

  ASSERT_EQ(fs.files.size(), 1);
  ASSERT_EQ(fs.files[0]->filename, "file.txt");
}

TEST(FileSetTest, FirstFileStartsAtZero) {
  FileSet fs;
  fs.add_file(File::from_source("a.sg", "abc"));

  ASSERT_EQ(fs.files[0]->base, 0u);
}

TEST(FileSetTest, LaterFilesFollowThePrevious) {
  FileSet fs;
  fs.add_file(File::from_source("a.sg", "abc"));
  fs.add_file(File::from_source("b.sg", "de"));
  fs.add_file(File::from_source("c.sg", "f"));

  ASSERT_EQ(fs.files[1]->base, 4u);
  ASSERT_EQ(fs.files[2]->base, 7u);
}

TEST(FileSetTest, PositionAt_ResolvesToTheOwningFile) {
  FileSet fs;
  fs.add_file(File::from_source("a.sg", "abc"));
  fs.add_file(File::from_source("b.sg", "de"));
  fs.files[1]->add_file_newline(1);

  auto in_a = fs.position_at(1);
  ASSERT_EQ(in_a.filename, "a.sg");
  ASSERT_EQ(in_a.line, 1u);
  ASSERT_EQ(in_a.column, 2u);

  auto in_b = fs.position_at(fs.files[1]->base + 1);
  ASSERT_EQ(in_b.filename, "b.sg");
  ASSERT_EQ(in_b.line, 2u);
  ASSERT_EQ(in_b.column, 1u);
}

// A span at a file's last byte must not spill into the next file.
TEST(FileSetTest, PositionAt_EofBelongsToTheFileItEnds) {
  FileSet fs;
  fs.add_file(File::from_source("a.sg", "abc"));
  fs.add_file(File::from_source("b.sg", "de"));

  ASSERT_EQ(fs.position_at(3).filename, "a.sg");
}

TEST(FileSetTest, PositionAt_EmptySetYieldsNoFile) {
  FileSet fs;

  ASSERT_EQ(fs.file_at(0), nullptr);
  ASSERT_TRUE(fs.position_at(0).filename.empty());
}

} // namespace saga

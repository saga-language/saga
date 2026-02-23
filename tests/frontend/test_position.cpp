#include "frontend/position.hpp"

#include <format>
#include <gtest/gtest.h>

namespace mc {

TEST(Position, Format) {
  Position p = {"file.txt", 1, 2};

  GTEST_ASSERT_EQ(std::format("{}", p), "file.txt:1:2");
}

} // namespace mc

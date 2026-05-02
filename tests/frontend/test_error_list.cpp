// Copywrite 2026 Rob Thornton
// SPDX-License-Identifier: MIT

#include "frontend/error_list.hpp"

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <sstream>

namespace saga {

TEST(ErrorList, ReportError_AppendsError) {
  Position p = {"file.txt", 1, 1};

  ErrorList el;

  GTEST_ASSERT_EQ(el.errors.size(), 0);

  el.report_error(p, "message");

  GTEST_ASSERT_EQ(el.errors.size(), 1);
}

TEST(ErrorList, MaxReached_FalseWhenEmpty) {
  ErrorList el;

  GTEST_ASSERT_FALSE(el.max_reached());
}

TEST(ErrorList, MaxReached_FalseWhenFull) {
  Position p = {"file.txt", 1, 1};

  ErrorList el;
  el.max_errors = 1;
  el.report_error(p, "message");

  GTEST_ASSERT_TRUE(el.max_reached());
}

TEST(ErrorList, PrintErrors_OutputsError) {
  Position p = {"file.txt", 1, 1};

  ErrorList el;
  el.report_error(p, "message");

  std::ostringstream oss;
  el.print_errors(oss);

  GTEST_ASSERT_EQ(oss.str(), "[file.txt:1:1] Error: message\n");
}

TEST(ErrorList, PrintErrors_OutputsMaxErrorMessage) {
  Position p = {"file.txt", 1, 1};

  ErrorList el;
  el.report_error(p, "message");
  el.max_errors = 1;

  std::ostringstream oss;
  el.print_errors(oss);

  std::string expected = "Max errors reached. Further messages suppressed.\n";
  ASSERT_THAT(oss.str(), testing::HasSubstr(expected));
}

TEST(ErrorList, PrintErrors_OutputsMultipleErrors) {
  Position p = {"file.txt", 1, 1};

  ErrorList el;
  el.report_error(p, "message1");
  el.report_error(p, "message2");
  el.max_errors = 1;

  std::ostringstream oss;
  el.print_errors(oss);

  ASSERT_THAT(oss.str(),
              testing::HasSubstr("[file.txt:1:1] Error: message1\n"));
  ASSERT_THAT(oss.str(),
              testing::HasSubstr("[file.txt:1:1] Error: message2\n"));
}

} // namespace saga

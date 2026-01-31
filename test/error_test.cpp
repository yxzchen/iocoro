#include <gtest/gtest.h>

#include <iocoro/error.hpp>

TEST(error_test, error_category_name_and_messages) {
  auto ec = iocoro::make_error_code(iocoro::error::not_open);
  EXPECT_STREQ(ec.category().name(), "iocoro");
  EXPECT_EQ(ec.message(), "resource not open");
}

TEST(error_test, make_error_code_is_equatable) {
  auto ec1 = iocoro::make_error_code(iocoro::error::busy);
  auto ec2 = iocoro::make_error_code(iocoro::error::busy);
  auto ec3 = iocoro::make_error_code(iocoro::error::eof);

  EXPECT_EQ(ec1, ec2);
  EXPECT_NE(ec1, ec3);
}

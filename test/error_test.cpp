#include <gtest/gtest.h>

#include <iocoro/error.hpp>

TEST(error_test, error_category_name_and_messages) {
  std::error_code const ec = iocoro::error::not_open;
  EXPECT_STREQ(ec.category().name(), "iocoro");
  EXPECT_EQ(ec.message(), "resource not open");
}

TEST(error_test, make_error_code_is_equatable) {
  std::error_code const ec1 = iocoro::error::busy;
  std::error_code const ec2 = iocoro::error::busy;
  std::error_code const ec3 = iocoro::error::eof;

  EXPECT_EQ(ec1, ec2);
  EXPECT_NE(ec1, ec3);
}

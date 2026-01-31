#include <gtest/gtest.h>

#include <iocoro/expected.hpp>

#include <string>

TEST(expected_test, value_and_error_basic) {
  iocoro::expected<int, int> ok{42};
  EXPECT_TRUE(ok);
  EXPECT_EQ(*ok, 42);

  iocoro::expected<int, int> err{ iocoro::unexpected<int>{7} };
  EXPECT_FALSE(err);
  EXPECT_EQ(err.error(), 7);
}

TEST(expected_test, move_and_copy_semantics) {
  iocoro::expected<std::string, int> v1{std::string{"hello"}};
  auto v2 = v1;
  EXPECT_TRUE(v2);
  EXPECT_EQ(*v2, "hello");

  auto v3 = std::move(v1);
  EXPECT_TRUE(v3);
  EXPECT_EQ(*v3, "hello");
}

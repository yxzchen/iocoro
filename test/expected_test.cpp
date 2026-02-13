#include <gtest/gtest.h>

#include <iocoro/expected.hpp>

#include <atomic>
#include <string>
#include <system_error>

TEST(expected_test, value_and_error_basic) {
  iocoro::expected<int, int> ok{42};
  EXPECT_TRUE(ok);
  EXPECT_EQ(*ok, 42);

  iocoro::expected<int, int> err{iocoro::unexpected<int>{7}};
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

TEST(expected_test, value_throws_bad_expected_access_on_error) {
  iocoro::expected<int, int> err{iocoro::unexpected<int>{7}};
  EXPECT_FALSE(err);
  try {
    (void)err.value();
    ADD_FAILURE() << "expected bad_expected_access";
  }
#if defined(__cpp_lib_expected) && __cpp_lib_expected >= 202211L
  catch (std::bad_expected_access<int> const& e) {
    EXPECT_EQ(e.error(), 7);
  }
#else
  catch (iocoro::bad_expected_access<int> const& e) {
    EXPECT_EQ(e.error(), 7);
  }
#endif
}

TEST(expected_test, value_or_ok_and_error) {
  iocoro::expected<int, int> ok{42};
  iocoro::expected<int, int> err{iocoro::unexpected<int>{7}};

  EXPECT_EQ(ok.value_or(1), 42);
  EXPECT_EQ(err.value_or(1), 1);
}

TEST(expected_test, and_then_transform_or_else_value_and_error_paths) {
  iocoro::expected<int, int> ok{3};
  iocoro::expected<int, int> err{iocoro::unexpected<int>{7}};

  auto ok2 = ok.and_then([](int v) { return iocoro::expected<int, int>(v + 1); });
  EXPECT_TRUE(ok2);
  EXPECT_EQ(*ok2, 4);

  auto err2 = err.and_then([](int v) { return iocoro::expected<int, int>(v + 1); });
  EXPECT_FALSE(err2);
  EXPECT_EQ(err2.error(), 7);

  auto ok3 = ok.transform([](int v) { return v * 2; });
  EXPECT_TRUE(ok3);
  EXPECT_EQ(*ok3, 6);

  auto err3 = err.transform([](int v) { return v * 2; });
  EXPECT_FALSE(err3);
  EXPECT_EQ(err3.error(), 7);

  std::atomic<int> or_else_calls{0};
  auto ok4 = ok.or_else([&](int) {
    or_else_calls.fetch_add(1, std::memory_order_relaxed);
    return iocoro::expected<int, int>(iocoro::unexpected<int>{9});
  });
  EXPECT_TRUE(ok4);
  EXPECT_EQ(*ok4, 3);
  EXPECT_EQ(or_else_calls.load(std::memory_order_relaxed), 0);

  auto err4 = err.or_else([&](int e) {
    or_else_calls.fetch_add(1, std::memory_order_relaxed);
    return iocoro::expected<int, int>(iocoro::unexpected<int>{e + 1});
  });
  EXPECT_FALSE(err4);
  EXPECT_EQ(err4.error(), 8);
  EXPECT_EQ(or_else_calls.load(std::memory_order_relaxed), 1);
}

TEST(expected_test, transform_can_return_void) {
  iocoro::expected<int, int> ok{3};
  iocoro::expected<int, int> err{iocoro::unexpected<int>{7}};

  int side_effect = 0;
  auto ok2 = ok.transform([&](int v) -> void { side_effect = v; });
  EXPECT_TRUE(ok2);
  EXPECT_EQ(side_effect, 3);

  // Error path should preserve error and not invoke the callable.
  side_effect = 0;
  auto err2 = err.transform([&](int v) -> void { side_effect = v; });
  EXPECT_FALSE(err2);
  EXPECT_EQ(err2.error(), 7);
  EXPECT_EQ(side_effect, 0);
}

TEST(expected_test, expected_void_and_then_transform_or_else) {
  iocoro::expected<void, int> ok{};
  iocoro::expected<void, int> err{iocoro::unexpected<int>{7}};

  auto ok2 = ok.and_then([] { return iocoro::expected<int, int>(42); });
  EXPECT_TRUE(ok2);
  EXPECT_EQ(*ok2, 42);

  auto err2 = err.and_then([] { return iocoro::expected<int, int>(42); });
  EXPECT_FALSE(err2);
  EXPECT_EQ(err2.error(), 7);

  auto ok3 = ok.transform([] { return 3; });
  EXPECT_TRUE(ok3);
  EXPECT_EQ(*ok3, 3);

  auto err3 = err.transform([] { return 3; });
  EXPECT_FALSE(err3);
  EXPECT_EQ(err3.error(), 7);

  std::atomic<int> or_else_calls{0};
  auto ok4 = ok.or_else([&](int) {
    or_else_calls.fetch_add(1, std::memory_order_relaxed);
    return iocoro::expected<void, int>(iocoro::unexpected<int>{9});
  });
  EXPECT_TRUE(ok4);
  EXPECT_EQ(or_else_calls.load(std::memory_order_relaxed), 0);

  auto err4 = err.or_else([&](int e) {
    or_else_calls.fetch_add(1, std::memory_order_relaxed);
    return iocoro::expected<void, int>(iocoro::unexpected<int>{e + 1});
  });
  EXPECT_FALSE(err4);
  EXPECT_EQ(err4.error(), 8);
  EXPECT_EQ(or_else_calls.load(std::memory_order_relaxed), 1);
}

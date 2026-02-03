#include <gtest/gtest.h>

#include <iocoro/co_sleep.hpp>
#include <iocoro/when_any.hpp>

#include "test_util.hpp"

#include <chrono>
#include <stdexcept>
#include <variant>
#include <vector>

TEST(when_any_test, variadic_returns_first_completed) {
  iocoro::io_context ctx;

  auto r = iocoro::test::sync_wait(
    ctx, [&]() -> iocoro::awaitable<std::pair<std::size_t, std::variant<int, int>>> {
      auto slow = []() -> iocoro::awaitable<int> {
        co_await iocoro::co_sleep(std::chrono::milliseconds{2});
        co_return 1;
      }();
      auto fast = []() -> iocoro::awaitable<int> {
        co_await iocoro::co_sleep(std::chrono::milliseconds{1});
        co_return 2;
      }();
      co_return co_await iocoro::when_any(std::move(slow), std::move(fast));
    }());

  ASSERT_TRUE(r);
  EXPECT_EQ(r->first, 1U);
  EXPECT_EQ(std::get<1>(r->second), 2);
}

TEST(when_any_test, container_returns_first_completed_with_index) {
  iocoro::io_context ctx;

  auto r = iocoro::test::sync_wait(ctx, [&]() -> iocoro::awaitable<std::pair<std::size_t, int>> {
    std::vector<iocoro::awaitable<int>> tasks;
    tasks.emplace_back([]() -> iocoro::awaitable<int> {
      co_await iocoro::co_sleep(std::chrono::milliseconds{2});
      co_return 1;
    }());
    tasks.emplace_back([]() -> iocoro::awaitable<int> {
      co_await iocoro::co_sleep(std::chrono::milliseconds{1});
      co_return 2;
    }());
    co_return co_await iocoro::when_any(std::move(tasks));
  }());

  ASSERT_TRUE(r);
  EXPECT_EQ(r->first, 1U);
  EXPECT_EQ(r->second, 2);
}

TEST(when_any_test, variadic_rethrows_exception_if_first) {
  iocoro::io_context ctx;

  auto r = iocoro::test::sync_wait(ctx, [&]() -> iocoro::awaitable<void> {
    auto bad = []() -> iocoro::awaitable<void> {
      throw std::runtime_error{"boom"};
      co_return;
    }();
    auto slow = []() -> iocoro::awaitable<void> {
      co_await iocoro::co_sleep(std::chrono::milliseconds{5});
      co_return;
    }();
    co_await iocoro::when_any(std::move(bad), std::move(slow));
  }());

  ASSERT_FALSE(r);
  ASSERT_TRUE(r.error());
}

TEST(when_any_test, variadic_single_element_returns_index_zero) {
  iocoro::io_context ctx;

  auto r = iocoro::test::sync_wait(
    ctx, [&]() -> iocoro::awaitable<std::pair<std::size_t, std::variant<int>>> {
      auto only = []() -> iocoro::awaitable<int> { co_return 7; }();
      co_return co_await iocoro::when_any(std::move(only));
    }());

  ASSERT_TRUE(r);
  EXPECT_EQ(r->first, 0U);
  EXPECT_EQ(std::get<0>(r->second), 7);
}

TEST(when_any_test, variadic_void_returns_monostate) {
  iocoro::io_context ctx;

  auto r = iocoro::test::sync_wait(
    ctx, [&]() -> iocoro::awaitable<std::pair<std::size_t, std::variant<std::monostate>>> {
      auto only = []() -> iocoro::awaitable<void> { co_return; }();
      co_return co_await iocoro::when_any(std::move(only));
    }());

  ASSERT_TRUE(r);
  EXPECT_EQ(r->first, 0U);
  EXPECT_EQ(r->second.index(), 0U);
}

TEST(when_any_test, container_single_element_returns_index_zero) {
  iocoro::io_context ctx;

  auto r = iocoro::test::sync_wait(ctx, [&]() -> iocoro::awaitable<std::pair<std::size_t, int>> {
    std::vector<iocoro::awaitable<int>> tasks;
    tasks.emplace_back([]() -> iocoro::awaitable<int> { co_return 9; }());
    co_return co_await iocoro::when_any(std::move(tasks));
  }());

  ASSERT_TRUE(r);
  EXPECT_EQ(r->first, 0U);
  EXPECT_EQ(r->second, 9);
}

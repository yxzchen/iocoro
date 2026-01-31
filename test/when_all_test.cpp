#include <gtest/gtest.h>

#include <iocoro/co_sleep.hpp>
#include <iocoro/when_all.hpp>

#include "test_util.hpp"

#include <chrono>
#include <string>
#include <tuple>
#include <vector>
#include <variant>

TEST(when_all_test, variadic_returns_tuple_and_preserves_order_and_monostate) {
  iocoro::io_context ctx;

  auto r = iocoro::test::sync_wait(
    ctx, [&]() -> iocoro::awaitable<std::tuple<int, std::monostate, std::string>> {
      auto a = []() -> iocoro::awaitable<int> { co_return 1; }();
      auto b = []() -> iocoro::awaitable<void> { co_return; }();
      auto c = []() -> iocoro::awaitable<std::string> { co_return std::string{"x"}; }();
      co_return co_await iocoro::when_all(std::move(a), std::move(b), std::move(c));
    }());

  ASSERT_TRUE(r);
  auto [i, m, s] = *r;
  EXPECT_EQ(i, 1);
  (void)m;
  EXPECT_EQ(s, "x");
}

TEST(when_all_test, container_returns_vector_and_preserves_order) {
  iocoro::io_context ctx;

  auto r = iocoro::test::sync_wait(
    ctx, [&]() -> iocoro::awaitable<std::vector<int>> {
      std::vector<iocoro::awaitable<int>> tasks;
      tasks.emplace_back([]() -> iocoro::awaitable<int> { co_return 1; }());
      tasks.emplace_back([]() -> iocoro::awaitable<int> { co_return 2; }());
      tasks.emplace_back([]() -> iocoro::awaitable<int> { co_return 3; }());
      co_return co_await iocoro::when_all(std::move(tasks));
    }());

  ASSERT_TRUE(r);
  ASSERT_EQ(r->size(), 3U);
  EXPECT_EQ((*r)[0], 1);
  EXPECT_EQ((*r)[1], 2);
  EXPECT_EQ((*r)[2], 3);
}

TEST(when_all_test, rethrows_first_exception_after_all_tasks_complete) {
  iocoro::io_context ctx;

  auto r = iocoro::test::sync_wait(
    ctx, [&]() -> iocoro::awaitable<void> {
      auto ok = []() -> iocoro::awaitable<void> {
        co_await iocoro::co_sleep(std::chrono::milliseconds{1});
        co_return;
      }();
      auto bad = []() -> iocoro::awaitable<void> {
        throw std::runtime_error{"boom"};
        co_return;
      }();
      co_await iocoro::when_all(std::move(ok), std::move(bad));
    }());

  ASSERT_FALSE(r);
  ASSERT_TRUE(r.error());
}

TEST(when_all_test, empty_container_returns_empty_vector) {
  iocoro::io_context ctx;

  auto r = iocoro::test::sync_wait(
    ctx, [&]() -> iocoro::awaitable<std::vector<int>> {
      std::vector<iocoro::awaitable<int>> tasks;
      co_return co_await iocoro::when_all(std::move(tasks));
    }());

  ASSERT_TRUE(r);
  EXPECT_TRUE(r->empty());
}

TEST(when_all_test, zero_variadic_returns_empty_tuple) {
  iocoro::io_context ctx;

  auto r = iocoro::test::sync_wait(
    ctx, [&]() -> iocoro::awaitable<std::tuple<>> {
      co_return co_await iocoro::when_all();
    }());

  ASSERT_TRUE(r);
}

TEST(when_all_test, container_single_element_returns_value) {
  iocoro::io_context ctx;

  auto r = iocoro::test::sync_wait(
    ctx, [&]() -> iocoro::awaitable<std::vector<int>> {
      std::vector<iocoro::awaitable<int>> tasks;
      tasks.emplace_back([]() -> iocoro::awaitable<int> { co_return 7; }());
      co_return co_await iocoro::when_all(std::move(tasks));
    }());

  ASSERT_TRUE(r);
  ASSERT_EQ(r->size(), 1U);
  EXPECT_EQ((*r)[0], 7);
}

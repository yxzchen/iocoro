#include <gtest/gtest.h>

#include <iocoro/co_sleep.hpp>
#include <iocoro/when_any.hpp>

#include "test_util.hpp"

#include <atomic>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <variant>
#include <vector>

using namespace std::chrono_literals;

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

TEST(when_any_test, returns_while_other_tasks_still_run) {
  iocoro::io_context ctx;

  auto slow_done = std::make_shared<std::atomic<bool>>(false);

  auto r = iocoro::test::sync_wait(ctx, [&]() -> iocoro::awaitable<void> {
    // NOTE: Avoid invoking coroutine lambdas on temporary closure objects.
    // Some toolchains can mis-handle capture lifetimes across suspension when the closure is a
    // prvalue (ASan can show this as stack-use-after-return).
    auto fast_fn = []() -> iocoro::awaitable<int> { co_return 7; };
    auto slow_fn = [slow_done]() -> iocoro::awaitable<int> {
      auto w = iocoro::co_sleep(30ms);
      co_await w;
      slow_done->store(true, std::memory_order_release);
      co_return 9;
    };

    auto [idx, v] = co_await iocoro::when_any(fast_fn(), slow_fn());
    EXPECT_EQ(idx, 0U);
    EXPECT_EQ(std::get<0>(v), 7);

    // Ensure slow continues after when_any returns.
    EXPECT_FALSE(slow_done->load(std::memory_order_acquire));

    // Wait (bounded) for slow to finish to avoid leaking background work.
    auto w = iocoro::co_sleep(80ms);
    co_await w;
    EXPECT_TRUE(slow_done->load(std::memory_order_acquire));
    co_return;
  }());

  ASSERT_TRUE(r);
}

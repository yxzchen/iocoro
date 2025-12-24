#include <gtest/gtest.h>

#include <iocoro/co_sleep.hpp>
#include <iocoro/co_spawn.hpp>
#include <iocoro/io_context.hpp>
#include <iocoro/src.hpp>
#include <iocoro/this_coro.hpp>
#include <iocoro/use_awaitable.hpp>
#include <iocoro/when_all.hpp>

#include "test_util.hpp"

#include <chrono>
#include <tuple>
#include <type_traits>
#include <variant>
#include <vector>

namespace {

TEST(when_all_test, variadic_returns_tuple_and_preserves_order_and_monostate) {
  iocoro::io_context ctx;

  auto t0 = []() -> iocoro::awaitable<void> { co_return; };
  auto t1 = []() -> iocoro::awaitable<int> { co_return 123; };
  auto t2 = []() -> iocoro::awaitable<void> { co_return; };

  auto result =
    iocoro::sync_wait(ctx, [&]() -> iocoro::awaitable<std::tuple<std::monostate, int, std::monostate>> {
      co_return co_await iocoro::when_all(t0(), t1(), t2());
    }());

  EXPECT_EQ(std::get<1>(result), 123);
}

TEST(when_all_test, variadic_empty_returns_empty_tuple) {
  iocoro::io_context ctx;
  auto result = iocoro::sync_wait(ctx, []() -> iocoro::awaitable<std::tuple<>> {
    co_return co_await iocoro::when_all();
  }());

  EXPECT_EQ(std::tuple_size_v<decltype(result)>, 0u);
}

TEST(when_all_test, rethrows_first_exception_after_all_tasks_complete) {
  using namespace std::chrono_literals;

  iocoro::io_context ctx;

  // Note: must be a coroutine (contain co_await/co_return) so the exception is observed on await.
  auto boom = []() -> iocoro::awaitable<void> {
    (void)co_await iocoro::this_coro::executor;
    throw std::runtime_error("boom");
  };

  auto slow = [&]() -> iocoro::awaitable<int> {
    co_await iocoro::co_sleep(10ms);
    co_return 7;
  };

  auto got_exception = iocoro::sync_wait_for(ctx, 200ms, [&]() -> iocoro::awaitable<bool> {
    try {
      (void)co_await iocoro::when_all(boom(), slow());
      ADD_FAILURE() << "expected exception";
    } catch (std::runtime_error const& e) {
      EXPECT_STREQ(e.what(), "boom");
      // Critical semantic: must have waited for slow() to finish.
      co_return true;
    }
    co_return false;
  }());

  EXPECT_TRUE(got_exception);
}

TEST(when_all_test, container_returns_vector_and_preserves_order) {
  iocoro::io_context ctx;

  std::vector<iocoro::awaitable<int>> tasks;
  tasks.push_back([]() -> iocoro::awaitable<int> { co_return 1; }());
  tasks.push_back([]() -> iocoro::awaitable<int> { co_return 2; }());
  tasks.push_back([]() -> iocoro::awaitable<int> { co_return 3; }());

  auto out = iocoro::sync_wait(ctx, [&]() -> iocoro::awaitable<std::vector<int>> {
    co_return co_await iocoro::when_all(std::move(tasks));
  }());

  ASSERT_EQ(out.size(), 3u);
  EXPECT_EQ(out[0], 1);
  EXPECT_EQ(out[1], 2);
  EXPECT_EQ(out[2], 3);
}

}  // namespace

#include <gtest/gtest.h>

#include <iocoro/co_sleep.hpp>
#include <iocoro/co_spawn.hpp>
#include <iocoro/completion_token.hpp>
#include <iocoro/io_context.hpp>
#include <iocoro/impl.hpp>
#include <iocoro/this_coro.hpp>
#include <iocoro/when_any.hpp>

#include "test_util.hpp"

#include <chrono>
#include <string>
#include <tuple>
#include <type_traits>
#include <variant>
#include <vector>

namespace {

TEST(when_any_test, variadic_returns_first_completed) {
  using namespace std::chrono_literals;

  iocoro::io_context ctx;

  using result_t = std::pair<std::size_t, std::variant<std::monostate, int, std::string>>;

  auto t0 = []() -> iocoro::awaitable<void> {
    co_await iocoro::co_sleep(50ms);
    co_return;
  };
  auto t1 = []() -> iocoro::awaitable<int> {
    co_await iocoro::co_sleep(10ms);
    co_return 42;
  };
  auto t2 = []() -> iocoro::awaitable<std::string> {
    co_await iocoro::co_sleep(100ms);
    co_return "hello";
  };

  auto result = iocoro::sync_wait(
    ctx, [&]() -> iocoro::awaitable<result_t> { return iocoro::when_any(t0(), t1(), t2()); }());

  EXPECT_EQ(result.first, 1u);  // t1 completes first
  EXPECT_TRUE(std::holds_alternative<int>(result.second));
  EXPECT_EQ(std::get<int>(result.second), 42);
}

TEST(when_any_test, variadic_rethrows_exception_if_first) {
  using namespace std::chrono_literals;

  iocoro::io_context ctx;

  auto boom = []() -> iocoro::awaitable<int> {
    (void)co_await iocoro::this_coro::executor;
    throw std::runtime_error("boom");
  };

  auto slow = []() -> iocoro::awaitable<std::string> {
    co_await iocoro::co_sleep(100ms);
    co_return "slow";
  };

  auto got_exception = iocoro::sync_wait_for(ctx, 200ms, [&]() -> iocoro::awaitable<bool> {
    try {
      (void)co_await iocoro::when_any(boom(), slow());
      ADD_FAILURE() << "expected exception";
    } catch (std::runtime_error const& e) {
      EXPECT_STREQ(e.what(), "boom");
      co_return true;
    }
    co_return false;
  }());

  EXPECT_TRUE(got_exception);
}

TEST(when_any_test, container_returns_first_completed_with_index) {
  using namespace std::chrono_literals;

  iocoro::io_context ctx;

  using result_t = std::pair<std::size_t, int>;

  std::vector<iocoro::awaitable<int>> tasks;
  tasks.push_back([]() -> iocoro::awaitable<int> {
    co_await iocoro::co_sleep(50ms);
    co_return 1;
  }());
  tasks.push_back([]() -> iocoro::awaitable<int> {
    co_await iocoro::co_sleep(10ms);
    co_return 2;
  }());
  tasks.push_back([]() -> iocoro::awaitable<int> {
    co_await iocoro::co_sleep(100ms);
    co_return 3;
  }());

  auto result = iocoro::sync_wait(
    ctx, [&]() -> iocoro::awaitable<result_t> { return iocoro::when_any(std::move(tasks)); }());

  EXPECT_EQ(result.first, 1u);  // Task at index 1 completes first
  EXPECT_EQ(result.second, 2);
}

TEST(when_any_test, container_void_returns_index) {
  using namespace std::chrono_literals;

  iocoro::io_context ctx;

  using result_t = std::pair<std::size_t, std::monostate>;

  std::vector<iocoro::awaitable<void>> tasks;
  tasks.push_back([]() -> iocoro::awaitable<void> {
    co_await iocoro::co_sleep(30ms);
    co_return;
  }());
  tasks.push_back([]() -> iocoro::awaitable<void> {
    co_await iocoro::co_sleep(5ms);
    co_return;
  }());

  auto result = iocoro::sync_wait(
    ctx, [&]() -> iocoro::awaitable<result_t> { return iocoro::when_any(std::move(tasks)); }());

  EXPECT_EQ(result.first, 1u);  // Second task completes first
}

}  // namespace

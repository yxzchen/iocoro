#include <gtest/gtest.h>

#include <xz/io/co_sleep.hpp>
#include <xz/io/co_spawn.hpp>
#include <xz/io/io_context.hpp>
#include <xz/io/src.hpp>
#include <xz/io/this_coro.hpp>
#include <xz/io/use_awaitable.hpp>
#include <xz/io/when_any.hpp>

#include <atomic>
#include <chrono>
#include <string>
#include <tuple>
#include <type_traits>
#include <variant>
#include <vector>

namespace {

TEST(when_any_test, variadic_returns_first_completed) {
  using namespace std::chrono_literals;

  xz::io::io_context ctx;
  auto ex = ctx.get_executor();

  std::atomic<bool> done{false};
  std::pair<std::size_t, std::variant<std::monostate, int, std::string>> result{};

  auto t0 = []() -> xz::io::awaitable<void> {
    co_await xz::io::co_sleep(50ms);
    co_return;
  };
  auto t1 = []() -> xz::io::awaitable<int> {
    co_await xz::io::co_sleep(10ms);
    co_return 42;
  };
  auto t2 = []() -> xz::io::awaitable<std::string> {
    co_await xz::io::co_sleep(100ms);
    co_return "hello";
  };

  auto parent = [&]() -> xz::io::awaitable<void> {
    result = co_await xz::io::when_any(t0(), t1(), t2());
    done.store(true, std::memory_order_relaxed);
    co_return;
  };

  xz::io::co_spawn(ex, parent(), xz::io::detached);
  (void)ctx.run();

  EXPECT_TRUE(done.load(std::memory_order_relaxed));
  EXPECT_EQ(result.first, 1u);  // t1 completes first
  EXPECT_TRUE(std::holds_alternative<int>(result.second));
  EXPECT_EQ(std::get<int>(result.second), 42);
}

TEST(when_any_test, variadic_rethrows_exception_if_first) {
  using namespace std::chrono_literals;

  xz::io::io_context ctx;
  auto ex = ctx.get_executor();

  std::atomic<bool> got_exception{false};

  auto boom = []() -> xz::io::awaitable<int> {
    (void)co_await xz::io::this_coro::executor;
    throw std::runtime_error("boom");
  };

  auto slow = []() -> xz::io::awaitable<std::string> {
    co_await xz::io::co_sleep(100ms);
    co_return "slow";
  };

  auto parent = [&]() -> xz::io::awaitable<void> {
    try {
      (void)co_await xz::io::when_any(boom(), slow());
      ADD_FAILURE() << "expected exception";
    } catch (std::runtime_error const& e) {
      EXPECT_STREQ(e.what(), "boom");
      got_exception.store(true, std::memory_order_relaxed);
    }
    co_return;
  };

  xz::io::co_spawn(ex, parent(), xz::io::detached);
  (void)ctx.run_for(200ms);

  EXPECT_TRUE(got_exception.load(std::memory_order_relaxed));
}

TEST(when_any_test, container_returns_first_completed_with_index) {
  using namespace std::chrono_literals;

  xz::io::io_context ctx;
  auto ex = ctx.get_executor();

  std::atomic<bool> done{false};
  std::pair<std::size_t, int> result{};

  std::vector<xz::io::awaitable<int>> tasks;
  tasks.push_back([]() -> xz::io::awaitable<int> {
    co_await xz::io::co_sleep(50ms);
    co_return 1;
  }());
  tasks.push_back([]() -> xz::io::awaitable<int> {
    co_await xz::io::co_sleep(10ms);
    co_return 2;
  }());
  tasks.push_back([]() -> xz::io::awaitable<int> {
    co_await xz::io::co_sleep(100ms);
    co_return 3;
  }());

  auto parent = [&]() -> xz::io::awaitable<void> {
    result = co_await xz::io::when_any(std::move(tasks));
    done.store(true, std::memory_order_relaxed);
    co_return;
  };

  xz::io::co_spawn(ex, parent(), xz::io::detached);
  (void)ctx.run();

  EXPECT_TRUE(done.load(std::memory_order_relaxed));
  EXPECT_EQ(result.first, 1u);  // Task at index 1 completes first
  EXPECT_EQ(result.second, 2);
}

TEST(when_any_test, container_void_returns_index) {
  using namespace std::chrono_literals;

  xz::io::io_context ctx;
  auto ex = ctx.get_executor();

  std::atomic<bool> done{false};
  std::pair<std::size_t, std::monostate> result{};

  std::vector<xz::io::awaitable<void>> tasks;
  tasks.push_back([]() -> xz::io::awaitable<void> {
    co_await xz::io::co_sleep(30ms);
    co_return;
  }());
  tasks.push_back([]() -> xz::io::awaitable<void> {
    co_await xz::io::co_sleep(5ms);
    co_return;
  }());

  auto parent = [&]() -> xz::io::awaitable<void> {
    result = co_await xz::io::when_any(std::move(tasks));
    done.store(true, std::memory_order_relaxed);
    co_return;
  };

  xz::io::co_spawn(ex, parent(), xz::io::detached);
  (void)ctx.run();

  EXPECT_TRUE(done.load(std::memory_order_relaxed));
  EXPECT_EQ(result.first, 1u);  // Second task completes first
}

}  // namespace


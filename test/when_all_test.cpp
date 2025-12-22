#include <gtest/gtest.h>

#include <xz/io/co_sleep.hpp>
#include <xz/io/co_spawn.hpp>
#include <xz/io/io_context.hpp>
#include <xz/io/src.hpp>
#include <xz/io/this_coro.hpp>
#include <xz/io/use_awaitable.hpp>
#include <xz/io/when_all.hpp>

#include <atomic>
#include <chrono>
#include <tuple>
#include <type_traits>
#include <variant>
#include <vector>

namespace {

TEST(when_all_test, variadic_returns_tuple_and_preserves_order_and_monostate) {
  xz::io::io_context ctx;
  auto ex = ctx.get_executor();

  std::atomic<bool> done{false};
  std::tuple<std::monostate, int, std::monostate> result{};

  auto t0 = []() -> xz::io::awaitable<void> { co_return; };
  auto t1 = []() -> xz::io::awaitable<int> { co_return 123; };
  auto t2 = []() -> xz::io::awaitable<void> { co_return; };

  auto parent = [&]() -> xz::io::awaitable<void> {
    result = co_await xz::io::when_all(t0(), t1(), t2());
    done.store(true, std::memory_order_relaxed);
    co_return;
  };

  xz::io::co_spawn(ex, parent(), xz::io::detached);
  (void)ctx.run();

  EXPECT_TRUE(done.load(std::memory_order_relaxed));
  EXPECT_EQ(std::get<1>(result), 123);
}

TEST(when_all_test, rethrows_first_exception_after_all_tasks_complete) {
  using namespace std::chrono_literals;

  xz::io::io_context ctx;
  auto ex = ctx.get_executor();

  std::atomic<bool> slow_done{false};
  std::atomic<bool> got_exception{false};

  // Note: must be a coroutine (contain co_await/co_return) so the exception is observed on await.
  auto boom = []() -> xz::io::awaitable<void> {
    (void)co_await xz::io::this_coro::executor;
    throw std::runtime_error("boom");
  };

  auto slow = [&]() -> xz::io::awaitable<int> {
    co_await xz::io::co_sleep(10ms);
    slow_done.store(true, std::memory_order_relaxed);
    co_return 7;
  };

  auto parent = [&]() -> xz::io::awaitable<void> {
    try {
      (void)co_await xz::io::when_all(boom(), slow());
      ADD_FAILURE() << "expected exception";
    } catch (std::runtime_error const& e) {
      EXPECT_STREQ(e.what(), "boom");
      // Critical semantic: must have waited for slow() to finish.
      EXPECT_TRUE(slow_done.load(std::memory_order_relaxed));
      got_exception.store(true, std::memory_order_relaxed);
    }
    co_return;
  };

  xz::io::co_spawn(ex, parent(), xz::io::detached);
  (void)ctx.run_for(200ms);

  EXPECT_TRUE(got_exception.load(std::memory_order_relaxed));
}

TEST(when_all_test, container_returns_vector_and_preserves_order) {
  xz::io::io_context ctx;
  auto ex = ctx.get_executor();

  std::atomic<bool> done{false};
  std::vector<int> out{};

  std::vector<xz::io::awaitable<int>> tasks;
  tasks.push_back([]() -> xz::io::awaitable<int> { co_return 1; }());
  tasks.push_back([]() -> xz::io::awaitable<int> { co_return 2; }());
  tasks.push_back([]() -> xz::io::awaitable<int> { co_return 3; }());

  auto parent = [&]() -> xz::io::awaitable<void> {
    out = co_await xz::io::when_all(std::move(tasks));
    done.store(true, std::memory_order_relaxed);
    co_return;
  };

  xz::io::co_spawn(ex, parent(), xz::io::detached);
  (void)ctx.run();

  EXPECT_TRUE(done.load(std::memory_order_relaxed));
  ASSERT_EQ(out.size(), 3u);
  EXPECT_EQ(out[0], 1);
  EXPECT_EQ(out[1], 2);
  EXPECT_EQ(out[2], 3);
}

}  // namespace



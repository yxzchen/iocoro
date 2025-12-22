#include <gtest/gtest.h>

#include <xz/io/co_sleep.hpp>
#include <xz/io/co_spawn.hpp>
#include <xz/io/io_context.hpp>
#include <xz/io/steady_timer.hpp>
#include <xz/io/src.hpp>
#include <xz/io/timer_handle.hpp>
#include <xz/io/use_awaitable.hpp>

#include <atomic>
#include <chrono>

namespace {

TEST(io_context_test, post_and_run_executes_all_posted_operations) {
  xz::io::io_context ctx;
  auto ex = ctx.get_executor();
  std::atomic<int> n{0};

  ex.post([&] { n.fetch_add(1, std::memory_order_relaxed); });
  ex.post([&] { n.fetch_add(1, std::memory_order_relaxed); });

  EXPECT_EQ(ctx.run(), 2U);
  EXPECT_EQ(n.load(std::memory_order_relaxed), 2);
}

TEST(io_context_test, run_one_does_not_drain_work_posted_during_execution) {
  xz::io::io_context ctx;
  auto ex = ctx.get_executor();
  std::atomic<int> n{0};

  ex.post([&] {
    n.fetch_add(1, std::memory_order_relaxed);
    ex.post([&] { n.fetch_add(1, std::memory_order_relaxed); });
  });

  EXPECT_EQ(ctx.run_one(), 1U);
  EXPECT_EQ(n.load(std::memory_order_relaxed), 1);

  EXPECT_EQ(ctx.run_one(), 1U);
  EXPECT_EQ(n.load(std::memory_order_relaxed), 2);
}

TEST(io_context_test, schedule_timer_runs_callback_via_run_for) {
  using namespace std::chrono_literals;

  xz::io::io_context ctx;
  auto ex = ctx.get_executor();
  std::atomic<bool> fired{false};

  (void)ex.schedule_timer(10ms, [&] { fired.store(true, std::memory_order_relaxed); });

  auto const n = ctx.run_for(200ms);
  EXPECT_GE(n, 1U);
  EXPECT_TRUE(fired.load(std::memory_order_relaxed));
}

TEST(io_context_test, stop_prevents_run_and_restart_allows_processing) {
  xz::io::io_context ctx;
  auto ex = ctx.get_executor();
  std::atomic<int> n{0};

  ctx.stop();
  ex.post([&] { n.fetch_add(1, std::memory_order_relaxed); });

  EXPECT_EQ(ctx.run(), 0U);
  EXPECT_EQ(n.load(std::memory_order_relaxed), 0);

  ctx.restart();
  EXPECT_EQ(ctx.run(), 1U);
  EXPECT_EQ(n.load(std::memory_order_relaxed), 1);
}

TEST(io_context_test, co_sleep_resumes_via_timer_and_executor) {
  using namespace std::chrono_literals;

  xz::io::io_context ctx;
  std::atomic<bool> done{false};

  auto task = [&]() -> xz::io::awaitable<void> {
    co_await xz::io::co_sleep(10ms);
    done.store(true, std::memory_order_relaxed);
  };

  xz::io::co_spawn(ctx.get_executor(), task());

  (void)ctx.run_for(200ms);
  EXPECT_TRUE(done.load(std::memory_order_relaxed));
}

TEST(io_context_test, steady_timer_async_wait_resumes_on_fire) {
  using namespace std::chrono_literals;

  xz::io::io_context ctx;
  auto ex = ctx.get_executor();
  std::atomic<bool> done{false};

  xz::io::steady_timer t{ex};
  t.expires_after(10ms);

  auto task = [&]() -> xz::io::awaitable<void> {
    co_await t.async_wait(xz::io::use_awaitable);
    done.store(true, std::memory_order_relaxed);
    co_return;
  };

  xz::io::co_spawn(ctx.get_executor(), task());

  (void)ctx.run_for(200ms);
  EXPECT_TRUE(done.load(std::memory_order_relaxed));
}

TEST(io_context_test, steady_timer_async_wait_resumes_on_cancel) {
  using namespace std::chrono_literals;

  xz::io::io_context ctx;
  auto ex = ctx.get_executor();
  std::atomic<bool> done{false};

  xz::io::steady_timer t{ex};
  t.expires_after(200ms);

  auto task = [&]() -> xz::io::awaitable<void> {
    co_await t.async_wait(xz::io::use_awaitable);
    done.store(true, std::memory_order_relaxed);
    co_return;
  };

  xz::io::co_spawn(ctx.get_executor(), task());

  // Let the coroutine start and suspend on async_wait, then cancel it.
  (void)ctx.run_one();
  (void)t.cancel();

  (void)ctx.run_for(50ms);
  EXPECT_TRUE(done.load(std::memory_order_relaxed));
}

}  // namespace



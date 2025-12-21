#include <gtest/gtest.h>

#include <xz/io/io_context.hpp>
#include <xz/io/src.hpp>

#include <atomic>
#include <chrono>

namespace {

TEST(io_context_test, post_and_run_executes_all_posted_operations) {
  xz::io::io_context ctx;
  std::atomic<int> n{0};

  ctx.post([&] { n.fetch_add(1, std::memory_order_relaxed); });
  ctx.post([&] { n.fetch_add(1, std::memory_order_relaxed); });

  EXPECT_EQ(ctx.run(), 2U);
  EXPECT_EQ(n.load(std::memory_order_relaxed), 2);
}

TEST(io_context_test, run_one_does_not_drain_work_posted_during_execution) {
  xz::io::io_context ctx;
  std::atomic<int> n{0};

  ctx.post([&] {
    n.fetch_add(1, std::memory_order_relaxed);
    ctx.post([&] { n.fetch_add(1, std::memory_order_relaxed); });
  });

  EXPECT_EQ(ctx.run_one(), 1U);
  EXPECT_EQ(n.load(std::memory_order_relaxed), 1);

  EXPECT_EQ(ctx.run_one(), 1U);
  EXPECT_EQ(n.load(std::memory_order_relaxed), 2);
}

TEST(io_context_test, schedule_timer_runs_callback_via_run_for) {
  using namespace std::chrono_literals;

  xz::io::io_context ctx;
  std::atomic<bool> fired{false};

  (void)ctx.schedule_timer(10ms, [&] { fired.store(true, std::memory_order_relaxed); });

  auto const n = ctx.run_for(200ms);
  EXPECT_GE(n, 1U);
  EXPECT_TRUE(fired.load(std::memory_order_relaxed));
}

TEST(io_context_test, stop_prevents_run_and_restart_allows_processing) {
  xz::io::io_context ctx;
  std::atomic<int> n{0};

  ctx.stop();
  ctx.post([&] { n.fetch_add(1, std::memory_order_relaxed); });

  EXPECT_EQ(ctx.run(), 0U);
  EXPECT_EQ(n.load(std::memory_order_relaxed), 0);

  ctx.restart();
  EXPECT_EQ(ctx.run(), 1U);
  EXPECT_EQ(n.load(std::memory_order_relaxed), 1);
}

}  // namespace



#include <gtest/gtest.h>

#include <iocoro/io_executor.hpp>
#include <iocoro/io_context.hpp>

#include <atomic>

namespace {

TEST(io_context_basic_test, post_and_run_executes_all_posted_operations) {
  iocoro::io_context ctx;
  auto ex = ctx.get_executor();
  std::atomic<int> n{0};

  ex.post([&] { n.fetch_add(1, std::memory_order_relaxed); });
  ex.post([&] { n.fetch_add(1, std::memory_order_relaxed); });

  EXPECT_EQ(ctx.run(), 2U);
  EXPECT_EQ(n.load(std::memory_order_relaxed), 2);
}

TEST(io_context_basic_test, run_one_does_not_drain_work_posted_during_execution) {
  iocoro::io_context ctx;
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

TEST(io_context_basic_test, stop_prevents_run_and_restart_allows_processing) {
  iocoro::io_context ctx;
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

}  // namespace

#include <gtest/gtest.h>

#include <iocoro/io_context.hpp>

#include <atomic>

TEST(io_context_test, post_and_run_executes_all_posted_operations) {
  iocoro::io_context ctx;
  auto ex = ctx.get_executor();

  std::atomic<int> count{0};
  ex.post([&] { ++count; });
  ex.post([&] { ++count; });

  ctx.run();
  EXPECT_EQ(count.load(), 2);
}

TEST(io_context_test, run_one_does_not_drain_work_posted_during_execution) {
  iocoro::io_context ctx;
  auto ex = ctx.get_executor();

  std::atomic<int> count{0};
  ex.post([&] {
    ++count;
    ex.post([&] { ++count; });
  });

  ctx.run_one();
  EXPECT_EQ(count.load(), 1);

  ctx.run_one();
  EXPECT_EQ(count.load(), 2);
}

TEST(io_context_test, stop_prevents_run_and_restart_allows_processing) {
  iocoro::io_context ctx;
  auto ex = ctx.get_executor();

  std::atomic<int> count{0};
  ex.post([&] { ++count; });

  ctx.stop();
  ctx.run();
  EXPECT_EQ(count.load(), 0);

  ctx.restart();
  ctx.run();
  EXPECT_EQ(count.load(), 1);
}

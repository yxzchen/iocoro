#include <gtest/gtest.h>

#include <iocoro/io_context.hpp>

#include <atomic>
#include <chrono>
#include <vector>

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

TEST(io_context_test, dispatch_runs_inline_on_context_thread) {
  iocoro::io_context ctx;
  auto ex = ctx.get_executor();

  std::vector<int> order;
  ex.post([&] {
    order.push_back(1);
    ex.dispatch([&] { order.push_back(2); });
    order.push_back(3);
  });

  ctx.run();
  ASSERT_EQ(order.size(), 3U);
  EXPECT_EQ(order[0], 1);
  EXPECT_EQ(order[1], 2);
  EXPECT_EQ(order[2], 3);
}

TEST(io_context_test, run_for_processes_posted_work) {
  iocoro::io_context ctx;
  auto ex = ctx.get_executor();

  std::atomic<int> count{0};
  ex.post([&] { ++count; });

  auto n = ctx.run_for(std::chrono::milliseconds{1});
  EXPECT_EQ(n, 1U);
  EXPECT_EQ(count.load(), 1);
}

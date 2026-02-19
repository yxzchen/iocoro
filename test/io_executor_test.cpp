#include <gtest/gtest.h>

#include <iocoro/any_executor.hpp>
#include <iocoro/any_io_executor.hpp>
#include <iocoro/io_context.hpp>
#include <iocoro/strand.hpp>

#include <atomic>
#include <vector>

TEST(io_executor_test, default_executor_is_empty) {
  iocoro::any_io_executor ex{};
  EXPECT_FALSE(static_cast<bool>(ex));
  EXPECT_TRUE(ex.stopped());
}

TEST(io_executor_test, context_provides_valid_executor) {
  iocoro::io_context ctx;
  auto ex = ctx.get_executor();

  EXPECT_TRUE(static_cast<bool>(ex));
  EXPECT_FALSE(ex.stopped());
}

TEST(io_executor_test, executors_from_same_context_are_equal) {
  iocoro::io_context ctx;
  auto ex1 = ctx.get_executor();
  auto ex2 = ctx.get_executor();

  EXPECT_EQ(ex1, ex2);
}

TEST(io_executor_test, post_queues_work) {
  iocoro::io_context ctx;
  auto ex = ctx.get_executor();

  std::atomic<int> count{0};
  ex.post([&] { ++count; });

  ctx.run();
  EXPECT_EQ(count.load(), 1);
}

TEST(io_executor_test, dispatch_posts_when_not_on_context_thread) {
  iocoro::io_context ctx;
  auto ex = ctx.get_executor();

  std::atomic<int> count{0};
  ex.dispatch([&] { ++count; });

  ctx.run();
  EXPECT_EQ(count.load(), 1);
}

TEST(io_executor_test, dispatch_runs_inline_on_context_thread) {
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

TEST(io_executor_test, any_io_executor_any_executor_roundtrip_preserves_equality) {
  iocoro::io_context ctx;
  auto ioex = ctx.get_executor();

  // any_io_executor <-> any_executor alternating round-trips.
  iocoro::any_executor e0 = iocoro::any_executor{ioex};
  iocoro::any_io_executor i1{e0};
  iocoro::any_executor e1{i1};
  iocoro::any_io_executor i2{e1};
  iocoro::any_executor e2{i2};

  EXPECT_EQ(e0, e1);
  EXPECT_EQ(e0, e2);
  EXPECT_EQ(i1, i2);
}

TEST(io_executor_test, any_io_executor_any_executor_roundtrip_preserves_equality_for_strand) {
  iocoro::io_context ctx;
  auto base = ctx.get_executor();

  auto strand = iocoro::make_strand(base);
  iocoro::any_executor e0{strand};
  iocoro::any_io_executor i1{e0};
  iocoro::any_executor e1{i1};
  iocoro::any_io_executor i2{e1};
  iocoro::any_executor e2{i2};

  EXPECT_EQ(e0, e1);
  EXPECT_EQ(e0, e2);
  EXPECT_EQ(i1, i2);
}

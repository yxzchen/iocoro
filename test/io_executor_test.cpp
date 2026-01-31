#include <gtest/gtest.h>

#include <iocoro/any_io_executor.hpp>
#include <iocoro/io_context.hpp>

#include <atomic>

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

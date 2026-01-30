#include <gtest/gtest.h>

#include <iocoro/any_io_executor.hpp>
#include <iocoro/io_context.hpp>
#include <iocoro/work_guard.hpp>

#include <atomic>
#include <chrono>
#include <thread>

namespace {

using namespace std::chrono_literals;

// Test IO executor creation and bool conversion
TEST(io_executor_test, default_executor_is_empty) {
  iocoro::any_io_executor ex;
  EXPECT_FALSE(ex);
}

TEST(io_executor_test, context_provides_valid_executor) {
  iocoro::io_context ctx;
  auto ex = ctx.get_executor();
  EXPECT_TRUE(ex);
}

// Test IO executor equality
TEST(io_executor_test, executors_from_same_context_are_equal) {
  iocoro::io_context ctx;
  auto ex1 = ctx.get_executor();
  auto ex2 = ctx.get_executor();
  EXPECT_EQ(ex1, ex2);
}

TEST(io_executor_test, executors_from_different_contexts_are_not_equal) {
  iocoro::io_context ctx1;
  iocoro::io_context ctx2;
  auto ex1 = ctx1.get_executor();
  auto ex2 = ctx2.get_executor();
  EXPECT_NE(ex1, ex2);
}

// Test post
TEST(io_executor_test, post_queues_work) {
  iocoro::io_context ctx;
  auto ex = ctx.get_executor();
  std::atomic<int> counter{0};

  ex.post([&counter] { counter.fetch_add(1, std::memory_order_relaxed); });
  ex.post([&counter] { counter.fetch_add(1, std::memory_order_relaxed); });

  EXPECT_EQ(counter.load(std::memory_order_relaxed), 0);
  ctx.run();
  EXPECT_EQ(counter.load(std::memory_order_relaxed), 2);
}

// Test dispatch
TEST(io_executor_test, dispatch_runs_inline_on_context_thread) {
  iocoro::io_context ctx;
  auto ex = ctx.get_executor();
  std::atomic<bool> ran_inline{false};
  std::atomic<bool> done{false};

  ex.post([&] {
    ex.dispatch([&] {
      ran_inline.store(true, std::memory_order_relaxed);
      done.store(true, std::memory_order_relaxed);
    });
  });

  ctx.run();
  EXPECT_TRUE(ran_inline.load(std::memory_order_relaxed));
  EXPECT_TRUE(done.load(std::memory_order_relaxed));
}

TEST(io_executor_test, dispatch_posts_from_different_thread) {
  iocoro::io_context ctx;
  auto ex = ctx.get_executor();
  std::atomic<bool> done{false};

  // Initialize the context thread
  ex.post([] {});
  ctx.run_one();

  // Call dispatch from a different thread
  std::thread t([&] { ex.dispatch([&] { done.store(true, std::memory_order_relaxed); }); });
  t.join();

  // Should have been posted, not executed inline
  EXPECT_FALSE(done.load(std::memory_order_relaxed));
  ctx.run();
  EXPECT_TRUE(done.load(std::memory_order_relaxed));
}

// Test stopped
TEST(io_executor_test, stopped_reflects_context_state) {
  iocoro::io_context ctx;
  auto ex = ctx.get_executor();

  EXPECT_FALSE(ex.stopped());

  ctx.stop();
  EXPECT_TRUE(ex.stopped());

  ctx.restart();
  EXPECT_FALSE(ex.stopped());
}

// Test work_guard
TEST(io_executor_test, work_guard_keeps_context_alive) {
  iocoro::io_context ctx;
  auto ex = ctx.get_executor();
  std::atomic<bool> work_done{false};
  std::atomic<bool> stop_guard{false};

  auto guard_ptr = std::make_shared<iocoro::work_guard<iocoro::any_io_executor>>(ex);

  std::thread t([&, guard_ptr] {
    std::this_thread::sleep_for(10ms);
    ex.post([&] { work_done.store(true, std::memory_order_relaxed); });
    std::this_thread::sleep_for(10ms);
    // Destroy the guard to allow run() to complete
    guard_ptr->reset();
  });

  // This would normally return immediately (no work), but the guard prevents that
  ctx.run();

  t.join();
  EXPECT_TRUE(work_done.load(std::memory_order_relaxed));
}

TEST(io_executor_test, work_guard_is_movable) {
  iocoro::io_context ctx;
  auto ex = ctx.get_executor();

  iocoro::work_guard<iocoro::any_io_executor> guard1(ex);
  iocoro::work_guard<iocoro::any_io_executor> guard2(std::move(guard1));

  EXPECT_TRUE(guard2.get_executor());

  iocoro::work_guard<iocoro::any_io_executor> guard3 = std::move(guard2);
  EXPECT_TRUE(guard3.get_executor());
}

// Test that IO executor can be copied and both copies refer to the same context
TEST(io_executor_test, executor_is_copyable) {
  iocoro::io_context ctx;
  auto ex1 = ctx.get_executor();
  auto ex2 = ex1;

  EXPECT_EQ(ex1, ex2);

  std::atomic<int> counter{0};
  ex1.post([&] { counter++; });
  ex2.post([&] { counter++; });

  ctx.run();
  EXPECT_EQ(counter.load(), 2);
}

}  // namespace

#include <gtest/gtest.h>

#include <xz/io/detail/context/io_context_impl.hpp>
#include <xz/io/src.hpp>

#include <atomic>
#include <chrono>
#include <thread>

namespace {

using namespace std::chrono_literals;

// Test basic construction and destruction
TEST(io_context_impl_basic, construct_and_destruct) {
  xz::io::detail::io_context_impl impl;
  EXPECT_FALSE(impl.stopped());
}

// Test post and process_posted
TEST(io_context_impl_basic, post_and_run_executes_operations) {
  xz::io::detail::io_context_impl impl;
  std::atomic<int> counter{0};

  impl.post([&counter] { counter.fetch_add(1, std::memory_order_relaxed); });
  impl.post([&counter] { counter.fetch_add(1, std::memory_order_relaxed); });

  auto const n = impl.run();
  EXPECT_EQ(n, 2U);
  EXPECT_EQ(counter.load(std::memory_order_relaxed), 2);
}

// Test run_one - note: process_posted() drains the entire queue
TEST(io_context_impl_basic, run_one_processes_batch) {
  xz::io::detail::io_context_impl impl;
  std::atomic<int> counter{0};

  impl.post([&counter] { counter.fetch_add(1, std::memory_order_relaxed); });
  impl.post([&counter] { counter.fetch_add(1, std::memory_order_relaxed); });

  // run_one() will process all posted operations in the queue as a batch
  auto const n = impl.run_one();
  EXPECT_EQ(n, 2U);
  EXPECT_EQ(counter.load(std::memory_order_relaxed), 2);

  // No more work
  EXPECT_EQ(impl.run_one(), 0U);
}

// Test stop and restart
TEST(io_context_impl_basic, stop_and_restart) {
  xz::io::detail::io_context_impl impl;
  std::atomic<int> counter{0};

  impl.stop();
  EXPECT_TRUE(impl.stopped());

  impl.post([&counter] { counter.fetch_add(1, std::memory_order_relaxed); });
  EXPECT_EQ(impl.run(), 0U);
  EXPECT_EQ(counter.load(std::memory_order_relaxed), 0);

  impl.restart();
  EXPECT_FALSE(impl.stopped());

  EXPECT_EQ(impl.run(), 1U);
  EXPECT_EQ(counter.load(std::memory_order_relaxed), 1);
}

// Test dispatch
TEST(io_context_impl_basic, dispatch_executes_inline_on_context_thread) {
  xz::io::detail::io_context_impl impl;
  std::atomic<int> counter{0};
  std::atomic<bool> executed_inline{false};

  impl.post([&] {
    impl.set_thread_id();
    impl.dispatch([&] {
      executed_inline.store(impl.running_in_this_thread(), std::memory_order_relaxed);
      counter.fetch_add(1, std::memory_order_relaxed);
    });
  });

  EXPECT_EQ(impl.run(), 1U);
  EXPECT_EQ(counter.load(std::memory_order_relaxed), 1);
  EXPECT_TRUE(executed_inline.load(std::memory_order_relaxed));
}

// Test dispatch posts when called from different thread
TEST(io_context_impl_basic, dispatch_posts_when_called_from_different_thread) {
  xz::io::detail::io_context_impl impl;
  std::atomic<int> counter{0};
  std::atomic<bool> done{false};

  impl.post([&] { impl.set_thread_id(); });
  impl.run_one();

  // Call dispatch from a different thread
  std::thread t([&] {
    impl.dispatch([&] {
      counter.fetch_add(1, std::memory_order_relaxed);
      done.store(true, std::memory_order_relaxed);
    });
  });
  t.join();

  // The dispatched work should be posted and require another run
  EXPECT_FALSE(done.load(std::memory_order_relaxed));
  EXPECT_EQ(impl.run(), 1U);
  EXPECT_TRUE(done.load(std::memory_order_relaxed));
  EXPECT_EQ(counter.load(std::memory_order_relaxed), 1);
}

// Test work guard keeps context running
TEST(io_context_impl_basic, work_guard_prevents_empty_run) {
  xz::io::detail::io_context_impl impl;

  impl.add_work_guard();

  std::thread t([&] {
    std::this_thread::sleep_for(10ms);
    impl.post([] {});
    impl.remove_work_guard();
  });

  // run() should block until work_guard is removed and the posted work is done
  auto const n = impl.run();
  t.join();

  EXPECT_EQ(n, 1U);
}

// Test timer scheduling
TEST(io_context_impl_timer, schedule_timer_executes_callback) {
  xz::io::detail::io_context_impl impl;
  std::atomic<bool> fired{false};

  auto entry = impl.schedule_timer(10ms, [&fired] {
    fired.store(true, std::memory_order_relaxed);
  });

  ASSERT_NE(entry, nullptr);
  EXPECT_TRUE(entry->is_pending());

  auto const n = impl.run_for(200ms);
  EXPECT_GE(n, 1U);
  EXPECT_TRUE(fired.load(std::memory_order_relaxed));
}

// Test timer cancellation
TEST(io_context_impl_timer, cancel_timer_prevents_execution) {
  xz::io::detail::io_context_impl impl;
  std::atomic<bool> fired{false};

  auto entry = impl.schedule_timer(100ms, [&fired] {
    fired.store(true, std::memory_order_relaxed);
  });

  ASSERT_NE(entry, nullptr);
  EXPECT_TRUE(entry->cancel());
  EXPECT_TRUE(entry->is_cancelled());

  (void)impl.run_for(200ms);
  EXPECT_FALSE(fired.load(std::memory_order_relaxed));
}

// Test multiple timers fire in order
TEST(io_context_impl_timer, multiple_timers_fire_in_order) {
  xz::io::detail::io_context_impl impl;
  std::atomic<int> counter{0};
  std::vector<int> order;

  auto e1 = impl.schedule_timer(30ms, [&] { order.push_back(1); counter++; });
  auto e2 = impl.schedule_timer(10ms, [&] { order.push_back(2); counter++; });
  auto e3 = impl.schedule_timer(20ms, [&] { order.push_back(3); counter++; });

  impl.run_for(200ms);

  EXPECT_EQ(counter.load(), 3);
  ASSERT_EQ(order.size(), 3U);
  EXPECT_EQ(order[0], 2);  // 10ms timer fires first
  EXPECT_EQ(order[1], 3);  // 20ms timer fires second
  EXPECT_EQ(order[2], 1);  // 30ms timer fires third
}

// Test run_for timeout
TEST(io_context_impl_basic, run_for_respects_timeout) {
  xz::io::detail::io_context_impl impl;
  impl.add_work_guard();

  auto start = std::chrono::steady_clock::now();
  impl.run_for(50ms);
  auto elapsed = std::chrono::steady_clock::now() - start;

  impl.remove_work_guard();

  // Should have waited approximately 50ms
  EXPECT_GE(elapsed, 40ms);  // Allow some tolerance
  EXPECT_LE(elapsed, 100ms);
}

}  // namespace


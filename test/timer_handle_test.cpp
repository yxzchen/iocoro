#include <gtest/gtest.h>

#include <iocoro/executor.hpp>
#include <iocoro/io_context.hpp>
#include <iocoro/impl.hpp>
#include <iocoro/timer_handle.hpp>

#include <atomic>
#include <chrono>
#include <memory>

namespace {

using namespace std::chrono_literals;

// Test timer_handle default construction
TEST(timer_handle_basic, default_constructed_handle_is_empty) {
  iocoro::timer_handle handle;
  EXPECT_FALSE(handle);
  EXPECT_FALSE(handle.pending());
  EXPECT_FALSE(handle.fired());
  EXPECT_FALSE(handle.cancelled());
}

// Test timer_handle created from executor
TEST(timer_handle_basic, handle_from_executor_is_valid) {
  iocoro::io_context ctx;
  auto ex = ctx.get_executor();

  auto handle = ex.schedule_timer(100ms, [] {});
  EXPECT_TRUE(handle);
  EXPECT_TRUE(handle.pending());
  EXPECT_FALSE(handle.fired());
  EXPECT_FALSE(handle.cancelled());
}

// Test timer_handle copy semantics
TEST(timer_handle_lifetime, handle_is_copyable) {
  iocoro::io_context ctx;
  auto ex = ctx.get_executor();

  auto handle1 = ex.schedule_timer(100ms, [] {});
  auto handle2 = handle1;

  EXPECT_TRUE(handle1);
  EXPECT_TRUE(handle2);
  EXPECT_TRUE(handle1.pending());
  EXPECT_TRUE(handle2.pending());

  // Cancel through one handle
  handle1.cancel();

  // Both should reflect the cancellation
  EXPECT_TRUE(handle1.cancelled());
  EXPECT_TRUE(handle2.cancelled());
}

// Test timer_handle survives after timer fires
TEST(timer_handle_lifetime, handle_survives_after_timer_fires) {
  iocoro::io_context ctx;
  auto ex = ctx.get_executor();
  std::atomic<bool> fired{false};

  auto handle = ex.schedule_timer(10ms, [&fired] { fired.store(true, std::memory_order_relaxed); });

  EXPECT_TRUE(handle);
  EXPECT_TRUE(handle.pending());

  ctx.run_for(200ms);

  // After firing, handle should still be valid and indicate fired state
  EXPECT_TRUE(handle);
  EXPECT_TRUE(handle.fired());
  EXPECT_FALSE(handle.pending());
  EXPECT_FALSE(handle.cancelled());
  EXPECT_TRUE(fired.load(std::memory_order_relaxed));
}

// Test multiple handles to the same timer
TEST(timer_handle_lifetime, multiple_handles_reference_same_timer) {
  iocoro::io_context ctx;
  auto ex = ctx.get_executor();
  std::atomic<int> fire_count{0};

  auto handle1 =
    ex.schedule_timer(10ms, [&fire_count] { fire_count.fetch_add(1, std::memory_order_relaxed); });
  auto handle2 = handle1;
  auto handle3 = handle2;

  ctx.run_for(200ms);

  // Timer should fire exactly once, even though we have multiple handles
  EXPECT_EQ(fire_count.load(std::memory_order_relaxed), 1);
  EXPECT_TRUE(handle1.fired());
  EXPECT_TRUE(handle2.fired());
  EXPECT_TRUE(handle3.fired());
}

// Test timer_handle destruction doesn't cancel timer
TEST(timer_handle_lifetime, handle_destruction_does_not_cancel_timer) {
  iocoro::io_context ctx;
  auto ex = ctx.get_executor();
  std::atomic<bool> fired{false};

  {
    auto handle =
      ex.schedule_timer(10ms, [&fired] { fired.store(true, std::memory_order_relaxed); });
    EXPECT_TRUE(handle.pending());
  }  // handle destroyed here

  // Timer should still fire even though handle was destroyed
  ctx.run_for(200ms);
  EXPECT_TRUE(fired.load(std::memory_order_relaxed));
}

// Test cancel prevents execution
TEST(timer_handle_cancel, cancel_prevents_timer_execution) {
  iocoro::io_context ctx;
  auto ex = ctx.get_executor();
  std::atomic<bool> fired{false};

  auto handle =
    ex.schedule_timer(100ms, [&fired] { fired.store(true, std::memory_order_relaxed); });

  handle.cancel();
  EXPECT_TRUE(handle.cancelled());

  ctx.run_for(200ms);
  EXPECT_FALSE(fired.load(std::memory_order_relaxed));
}

// Test cancel on already fired timer
TEST(timer_handle_cancel, cancel_after_fire_is_noop) {
  iocoro::io_context ctx;
  auto ex = ctx.get_executor();
  std::atomic<bool> fired{false};

  auto handle = ex.schedule_timer(10ms, [&fired] { fired.store(true, std::memory_order_relaxed); });

  ctx.run_for(200ms);
  EXPECT_TRUE(handle.fired());
  EXPECT_TRUE(fired.load(std::memory_order_relaxed));

  // Cancel after fire should be a no-op
  auto result = handle.cancel();
  EXPECT_EQ(result, 0U);
  EXPECT_TRUE(handle.fired());  // Still fired, not cancelled
}

// Test cancel on already cancelled timer
TEST(timer_handle_cancel, double_cancel_is_safe) {
  iocoro::io_context ctx;
  auto ex = ctx.get_executor();

  auto handle = ex.schedule_timer(100ms, [] {});

  (void)handle.cancel();
  EXPECT_TRUE(handle.cancelled());

  // Second cancel should be a no-op
  auto result2 = handle.cancel();
  EXPECT_EQ(result2, 0U);
  EXPECT_TRUE(handle.cancelled());
}

// Test that timer_entry is kept alive by handle even after context processes it
TEST(timer_handle_lifetime, handle_keeps_entry_alive_after_processing) {
  iocoro::io_context ctx;
  auto ex = ctx.get_executor();
  std::atomic<bool> fired{false};

  auto handle = ex.schedule_timer(10ms, [&fired] { fired.store(true, std::memory_order_relaxed); });

  // Let the timer fire and be processed
  ctx.run_for(200ms);
  EXPECT_TRUE(fired.load(std::memory_order_relaxed));

  // Handle should still be valid and show fired state
  EXPECT_TRUE(handle);
  EXPECT_TRUE(handle.fired());
  EXPECT_FALSE(handle.pending());
  EXPECT_FALSE(handle.cancelled());

  // This shouldn't crash - the entry should still be alive
  handle.cancel();  // This is a no-op since it's already fired
}

// Test timer_handle with short-lived context
TEST(timer_handle_lifetime, handle_outlives_context_safely) {
  std::shared_ptr<iocoro::timer_handle> handle_ptr;
  std::atomic<bool> fired{false};

  {
    iocoro::io_context ctx;
    auto ex = ctx.get_executor();

    handle_ptr = std::make_shared<iocoro::timer_handle>(
      ex.schedule_timer(10ms, [&fired] { fired.store(true, std::memory_order_relaxed); }));

    ctx.run_for(200ms);
    EXPECT_TRUE(fired.load(std::memory_order_relaxed));
  }  // ctx destroyed here

  // Handle should still be valid even though context is gone
  EXPECT_TRUE(*handle_ptr);
  EXPECT_TRUE(handle_ptr->fired());

  // Operations on the handle should not crash
  handle_ptr->cancel();  // No-op, but shouldn't crash
  EXPECT_TRUE(handle_ptr->fired());
}

}  // namespace

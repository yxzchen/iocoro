#include <xz/io/io.hpp>
#include <xz/io/src.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <thread>

using namespace std::chrono_literals;

TEST(IoContextTest, BasicConstruction) {
  xz::io::io_context ctx;
  EXPECT_FALSE(ctx.stopped());
}

TEST(IoContextTest, PostAndRun) {
  xz::io::io_context ctx;
  int counter = 0;

  ctx.post([&counter]() { counter++; });
  ctx.post([&counter]() { counter++; });
  ctx.post([&counter]() { counter += 3; });

  auto count = ctx.run();
  EXPECT_EQ(count, 3);
  EXPECT_EQ(counter, 5);
}

TEST(IoContextTest, RunFor) {
  xz::io::io_context ctx;
  bool executed = false;

  ctx.post([&executed]() { executed = true; });

  auto count = ctx.run_for(100ms);
  EXPECT_EQ(count, 1);
  EXPECT_TRUE(executed);
}

TEST(IoContextTest, StopAndRestart) {
  xz::io::io_context ctx;
  int counter = 0;

  ctx.post([&ctx]() { ctx.stop(); });
  ctx.post([&counter]() { counter++; });

  ctx.run();
  EXPECT_TRUE(ctx.stopped());
  EXPECT_EQ(counter, 0);

  ctx.restart();
  EXPECT_FALSE(ctx.stopped());
  ctx.run();
  EXPECT_EQ(counter, 1);
}

TEST(IoContextTest, Timer) {
  xz::io::io_context ctx;
  bool timer_fired = false;

  auto handle = ctx.schedule_timer(50ms, [&timer_fired]() { timer_fired = true; });

  auto start = std::chrono::steady_clock::now();
  ctx.run_for(200ms);
  auto elapsed = std::chrono::steady_clock::now() - start;

  EXPECT_TRUE(timer_fired);
  EXPECT_GE(elapsed, 49ms);
  EXPECT_LT(elapsed, 150ms);
}

TEST(IoContextTest, CancelTimer) {
  xz::io::io_context ctx;
  bool timer_fired = false;

  auto handle = ctx.schedule_timer(50ms, [&timer_fired]() { timer_fired = true; });
  ctx.cancel_timer(handle);

  ctx.run_for(100ms);
  EXPECT_FALSE(timer_fired);
}

TEST(IoContextTest, MultipleTimers) {
  xz::io::io_context ctx;
  int counter = 0;

  ctx.schedule_timer(20ms, [&counter]() { counter += 1; });
  ctx.schedule_timer(40ms, [&counter]() { counter += 10; });
  ctx.schedule_timer(60ms, [&counter]() { counter += 100; });

  ctx.run_for(100ms);
  EXPECT_EQ(counter, 111);
}

TEST(IoContextTest, DispatchOnSameThread) {
  xz::io::io_context ctx;
  int counter = 0;

  ctx.post([&ctx, &counter]() {
    // Inside event loop, dispatch should execute immediately
    ctx.dispatch([&counter]() { counter = 42; });
    EXPECT_EQ(counter, 42);
  });

  ctx.run();
}

TEST(IoContextTest, WorkGuardWithSeparateThread) {
  xz::io::io_context ctx;
  std::atomic<bool> event_loop_running{false};
  std::atomic<bool> event_loop_exited{false};

  std::thread t2;
  {
    xz::io::work_guard<xz::io::io_context> guard(ctx);

    t2 = std::thread([&]() {
      event_loop_running.store(true);
      ctx.run();  // Will keep running because of work guard
      event_loop_exited.store(true);
    });

    // Give event loop time to start
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    EXPECT_TRUE(event_loop_running.load());
    EXPECT_FALSE(event_loop_exited.load());  // Should still be running

    // Verify work guard is keeping it alive by posting a task
    std::atomic<bool> task_executed{false};
    ctx.post([&task_executed]() { task_executed.store(true); });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_TRUE(task_executed.load());

    // Destroy work guard (goes out of scope)
  }

  // Give event loop time to exit after work guard is removed
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  EXPECT_TRUE(event_loop_exited.load());

  t2.join();
}

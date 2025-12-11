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

TEST(IoContextTest, WorkGuardClassPreventsExit) {
  xz::io::io_context ctx;
  std::atomic<bool> ran{false};

  {
    xz::io::work_guard<xz::io::io_context> guard(ctx);

    ctx.post([&ran]() { ran.store(true); });

    // Run with timeout - should keep running due to work guard
    auto start = std::chrono::steady_clock::now();
    ctx.run_for(100ms);
    auto elapsed = std::chrono::steady_clock::now() - start;

    // Should have run for approximately 100ms (not immediately exited)
    EXPECT_GE(elapsed, std::chrono::milliseconds(90));
    EXPECT_TRUE(ran.load());
  }

  // Work guard destroyed, event loop can now exit
  ran.store(false);
  auto start = std::chrono::steady_clock::now();
  ctx.run_for(10ms);
  auto elapsed = std::chrono::steady_clock::now() - start;

  // Should exit quickly
  EXPECT_LT(elapsed, std::chrono::milliseconds(20));
  EXPECT_FALSE(ran.load());
}

TEST(IoContextTest, WorkGuardRemovalWakesEventLoop) {
  xz::io::io_context ctx;

  // Add a work guard and start the event loop in a background thread
  {
    xz::io::work_guard<xz::io::io_context> guard(ctx);

    std::atomic<bool> running{true};
    std::atomic<bool> exited{false};

    std::thread t([&]() {
      while (running) {
        ctx.run_one();
      }
      exited.store(true);
    });

    // Give the event loop time to start
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    // Remove the work guard - this should wake up the event loop
    guard.~work_guard();

    // Give it time to exit
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    running.store(false);
    ctx.run_one();  // Wake it up one more time to process the stop

    t.join();

    EXPECT_TRUE(exited.load());
  }
}

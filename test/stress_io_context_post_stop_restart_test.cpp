#include <gtest/gtest.h>

#include <iocoro/io_context.hpp>
#include <iocoro/work_guard.hpp>

#include <atomic>
#include <chrono>
#include <thread>

TEST(stress_io_context, concurrent_post_and_stop_restart_does_not_deadlock) {
  using namespace std::chrono_literals;

  iocoro::io_context ctx;
  auto ex = ctx.get_executor();

  auto guard = iocoro::make_work_guard(ctx);

  std::atomic<bool> done{false};
  std::atomic<int> posted{0};
  std::atomic<int> executed{0};

  std::thread runner([&] {
    while (!done.load(std::memory_order_acquire)) {
      (void)ctx.run_for(std::chrono::milliseconds{1});
      std::this_thread::yield();
    }
  });

  std::thread producer([&] {
    for (int i = 0; i < 20000; ++i) {
      posted.fetch_add(1, std::memory_order_relaxed);
      ex.post([&] { executed.fetch_add(1, std::memory_order_relaxed); });
      if ((i % 128) == 0) {
        std::this_thread::yield();
      }
    }
  });

  std::thread toggler([&] {
    for (int i = 0; i < 2000; ++i) {
      ctx.stop();
      std::this_thread::sleep_for(50us);
      ctx.restart();
      if ((i % 16) == 0) {
        std::this_thread::yield();
      }
    }
  });

  producer.join();
  toggler.join();

  done.store(true, std::memory_order_release);
  runner.join();

  // Drain remaining work on the main thread (single event loop thread at a time).
  auto const deadline = std::chrono::steady_clock::now() + 2s;
  while (std::chrono::steady_clock::now() < deadline) {
    ctx.restart();
    (void)ctx.run_for(std::chrono::milliseconds{1});
    if (executed.load(std::memory_order_relaxed) == posted.load(std::memory_order_relaxed)) {
      break;
    }
    std::this_thread::sleep_for(1ms);
  }

  EXPECT_EQ(executed.load(std::memory_order_relaxed), posted.load(std::memory_order_relaxed));
}


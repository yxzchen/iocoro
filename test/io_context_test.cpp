#include <gtest/gtest.h>

#include <iocoro/co_spawn.hpp>
#include <iocoro/io_context.hpp>
#include <iocoro/steady_timer.hpp>
#include <iocoro/work_guard.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

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

TEST(io_context_test, run_one_single_turn_may_complete_multiple_ready_callbacks) {
  iocoro::io_context ctx;
  auto ex = ctx.get_executor();

  std::atomic<int> count{0};
  ex.post([&] { ++count; });
  ex.post([&] { ++count; });
  ex.post([&] { ++count; });

  auto n = ctx.run_one();
  EXPECT_EQ(n, 3U);
  EXPECT_EQ(count.load(), 3);
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

TEST(io_context_test, dispatch_on_context_thread_is_post_semantics) {
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
  EXPECT_EQ(order[1], 3);
  EXPECT_EQ(order[2], 2);
}

TEST(io_context_test, dispatch_throwing_callback_does_not_terminate_noexcept_call_site) {
  ::testing::GTEST_FLAG(death_test_style) = "threadsafe";

  ASSERT_EXIT(
    {
      iocoro::io_context ctx;
      auto ex = ctx.get_executor();

      std::vector<int> order;
      ex.post([&] {
        order.push_back(1);
        ex.dispatch([&] {
          order.push_back(2);
          throw std::runtime_error{"dispatch failure"};
        });
        order.push_back(3);
      });

      bool threw = false;
      try {
        (void)ctx.run();
      } catch (std::runtime_error const&) {
        threw = true;
      }

      if (!threw || order.size() != 3U || order[0] != 1 || order[1] != 3 || order[2] != 2) {
        std::_Exit(1);
      }
      std::_Exit(0);
    },
    ::testing::ExitedWithCode(0), ".*");
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

TEST(io_context_test, stop_preserves_posted_until_restart) {
  iocoro::io_context ctx;
  auto ex = ctx.get_executor();

  std::atomic<int> count{0};
  ctx.stop();
  for (int i = 0; i < 100; ++i) {
    ex.post([&] { count.fetch_add(1, std::memory_order_relaxed); });
  }

  // While stopped, we should not execute user callbacks.
  for (int i = 0; i < 10; ++i) {
    (void)ctx.run_for(1ms);
    EXPECT_EQ(count.load(std::memory_order_relaxed), 0);
  }

  ctx.restart();
  ctx.run();
  EXPECT_EQ(count.load(std::memory_order_relaxed), 100);
}

TEST(io_context_test, posted_fairness_does_not_starve_expired_timer) {
  iocoro::io_context ctx;
  auto ex = ctx.get_executor();

  std::atomic<int> fired{0};
  iocoro::steady_timer t{ex};
  t.expires_after(0ms);

  // Post more than max_drain_per_tick so posted_queue will yield.
  for (std::size_t i = 0; i < 2048; ++i) {
    ex.post([] {});
  }

  iocoro::co_spawn(
    ex,
    [&]() -> iocoro::awaitable<void> {
      auto r = co_await t.async_wait(iocoro::use_awaitable);
      EXPECT_TRUE(static_cast<bool>(r));
      fired.fetch_add(1, std::memory_order_relaxed);
      co_return;
    },
    iocoro::detached);

  // The timer should fire without requiring the posted queue to fully drain.
  auto const deadline = std::chrono::steady_clock::now() + 1s;
  while (std::chrono::steady_clock::now() < deadline) {
    (void)ctx.run_one();
    if (fired.load(std::memory_order_relaxed) == 1) {
      break;
    }
  }
  EXPECT_EQ(fired.load(std::memory_order_relaxed), 1);
}

TEST(io_context_test, stress_concurrent_post_and_stop_restart_does_not_deadlock) {
  iocoro::io_context ctx;
  auto ex = ctx.get_executor();

  auto guard = iocoro::make_work_guard(ctx);

  std::atomic<bool> done{false};
  std::atomic<int> posted{0};
  std::atomic<int> executed{0};

  std::thread runner([&] {
    while (!done.load(std::memory_order_acquire)) {
      (void)ctx.run_for(1ms);
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

  auto const deadline = std::chrono::steady_clock::now() + 2s;
  while (std::chrono::steady_clock::now() < deadline) {
    ctx.restart();
    (void)ctx.run_for(1ms);
    if (executed.load(std::memory_order_relaxed) == posted.load(std::memory_order_relaxed)) {
      break;
    }
    std::this_thread::sleep_for(1ms);
  }

  EXPECT_EQ(executed.load(std::memory_order_relaxed), posted.load(std::memory_order_relaxed));
}

TEST(io_context_test, stopped_context_does_not_fire_expired_timer_until_restart) {
  iocoro::io_context ctx;
  auto ex = ctx.get_executor();

  std::atomic<bool> fired{false};
  iocoro::steady_timer t{ex};
  t.expires_after(50ms);

  iocoro::co_spawn(
    ex,
    [&]() -> iocoro::awaitable<void> {
      auto r = co_await t.async_wait(iocoro::use_awaitable);
      if (r) {
        fired.store(true, std::memory_order_release);
      }
      co_return;
    },
    iocoro::detached);

  // Start the coroutine and ensure the timer is registered.
  (void)ctx.run_one();

  ctx.stop();
  std::this_thread::sleep_for(60ms);

  // While stopped, run_for should not make progress on timers.
  for (int i = 0; i < 5; ++i) {
    (void)ctx.run_for(1ms);
    EXPECT_FALSE(fired.load(std::memory_order_acquire));
  }

  ctx.restart();
  ctx.run();
  EXPECT_TRUE(fired.load(std::memory_order_acquire));
}

TEST(io_context_test, dispatch_while_stopped_is_not_inline) {
  iocoro::io_context ctx;
  auto ex = ctx.get_executor();

  std::vector<int> order;

  ex.post([&] {
    order.push_back(1);

    ctx.stop();
    ex.dispatch([&] { order.push_back(2); });

    order.push_back(3);
  });

  ctx.run();

  ASSERT_EQ(order.size(), 2U);
  EXPECT_EQ(order[0], 1);
  EXPECT_EQ(order[1], 3);

  ctx.restart();
  ctx.run();

  ASSERT_EQ(order.size(), 3U);
  EXPECT_EQ(order[2], 2);
}

TEST(io_context_test, dispatch_after_run_exit_is_not_inline_until_next_run) {
  iocoro::io_context ctx;
  auto ex = ctx.get_executor();

  std::vector<int> order;
  ex.post([&] { order.push_back(1); });
  ctx.run();

  ex.dispatch([&] { order.push_back(2); });
  ASSERT_EQ(order.size(), 1U);

  ctx.run();
  ASSERT_EQ(order.size(), 2U);
  EXPECT_EQ(order[0], 1);
  EXPECT_EQ(order[1], 2);
}

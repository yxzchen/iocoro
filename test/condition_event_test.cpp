#include <gtest/gtest.h>

#include <iocoro/co_sleep.hpp>
#include <iocoro/iocoro.hpp>
#include <iocoro/with_timeout.hpp>

#include "test_util.hpp"

#include <atomic>
#include <chrono>
#include <stop_token>
#include <thread>
#include <vector>

namespace iocoro::test {

using namespace std::chrono_literals;

TEST(condition_event_test, notify_before_wait_not_lost) {
  iocoro::io_context ctx;
  iocoro::condition_event ev;

  auto task = [&]() -> iocoro::awaitable<void> {
    ev.notify();
    auto r = co_await ev.async_wait();
    EXPECT_TRUE(r);
  };

  auto r = iocoro::test::sync_wait(ctx, task());
  ASSERT_TRUE(r);
}

TEST(condition_event_test, accumulates_pending) {
  iocoro::io_context ctx;
  iocoro::condition_event ev;

  auto task = [&]() -> iocoro::awaitable<void> {
    ev.notify();
    ev.notify();

    auto r1 = co_await ev.async_wait();
    auto r2 = co_await ev.async_wait();

    EXPECT_TRUE(r1);
    EXPECT_TRUE(r2);
  };

  auto r = iocoro::test::sync_wait(ctx, task());
  ASSERT_TRUE(r);
}

TEST(condition_event_test, notify_wakes_exactly_one) {
  iocoro::io_context ctx;
  iocoro::condition_event ev;
  std::atomic<int> woke{0};

  auto waiter = [&]() -> iocoro::awaitable<void> {
    auto r = co_await ev.async_wait();
    if (r) {
      woke.fetch_add(1);
    }
  };

  auto task = [&]() -> iocoro::awaitable<void> {
    auto ex = co_await iocoro::this_coro::executor;

    auto a1 = iocoro::co_spawn(ex, waiter(), iocoro::use_awaitable);
    auto a2 = iocoro::co_spawn(ex, waiter(), iocoro::use_awaitable);

    // Let both spawned coroutines run and suspend on the event.
    co_await iocoro::co_sleep(2ms);

    ev.notify();
    co_await iocoro::co_sleep(2ms);
    EXPECT_EQ(woke.load(), 1);

    ev.notify();
    (void)co_await iocoro::when_all(std::move(a1), std::move(a2));
    EXPECT_EQ(woke.load(), 2);
  };

  auto r = iocoro::test::sync_wait(ctx, task());
  ASSERT_TRUE(r);
}

TEST(condition_event_test, notify_many_wakes_all) {
  iocoro::io_context ctx;
  iocoro::condition_event ev;

  constexpr int n = 8;
  std::atomic<int> woke{0};

  auto waiter = [&]() -> iocoro::awaitable<void> {
    auto r = co_await ev.async_wait();
    if (r) {
      woke.fetch_add(1);
    }
  };

  auto task = [&]() -> iocoro::awaitable<void> {
    auto ex = co_await iocoro::this_coro::executor;

    std::vector<iocoro::awaitable<void>> tasks;
    tasks.reserve(n);
    for (int i = 0; i < n; ++i) {
      tasks.push_back(iocoro::co_spawn(ex, waiter(), iocoro::use_awaitable));
    }

    co_await iocoro::co_sleep(2ms);

    for (int i = 0; i < n; ++i) {
      ev.notify();
    }

    (void)co_await iocoro::when_all(std::move(tasks));
    EXPECT_EQ(woke.load(), n);
  };

  auto r = iocoro::test::sync_wait(ctx, task());
  ASSERT_TRUE(r);
}

TEST(condition_event_test, stop_cancels_and_removes_waiter) {
  iocoro::io_context ctx;
  iocoro::condition_event ev;

  auto task = [&]() -> iocoro::awaitable<void> {
    auto ex = co_await iocoro::this_coro::executor;
    std::stop_source stop_src{};

    std::jthread stopper{[&]() {
      std::this_thread::sleep_for(2ms);
      stop_src.request_stop();
    }};

    auto w = iocoro::co_spawn(
      ex, stop_src.get_token(),
      [&]() -> iocoro::awaitable<iocoro::result<void>> { co_return co_await ev.async_wait(); },
      iocoro::use_awaitable);

    auto r = co_await std::move(w);
    EXPECT_FALSE(r);
    EXPECT_EQ(r.error(), iocoro::error::operation_aborted);

    // If the waiter wasn't removed, this notify would be "consumed" by the cancelled waiter.
    // Instead, it should become pending and be consumed by the next wait.
    ev.notify();
    auto r2 = co_await ev.async_wait();
    EXPECT_TRUE(r2);
  };

  auto r = iocoro::test::sync_wait(ctx, task());
  ASSERT_TRUE(r);
}

TEST(condition_event_test, destroy_aborts_waiters) {
  iocoro::io_context ctx;
  std::error_code const aborted_ec = iocoro::error::operation_aborted;

  auto task = [&]() -> iocoro::awaitable<void> {
    auto* ev = new iocoro::condition_event{};
    std::atomic<bool> started{false};

    std::jthread killer{[&]() {
      while (!started.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      std::this_thread::sleep_for(2ms);
      delete ev;
    }};

    started.store(true, std::memory_order_release);

    auto r = co_await ev->async_wait();
    EXPECT_FALSE(r);
    EXPECT_EQ(r.error(), aborted_ec);
  };

  auto r = iocoro::test::sync_wait(ctx, task());
  ASSERT_TRUE(r);
}

TEST(condition_event_test, stress_notify_from_many_threads_wakes_exactly_all_waiters) {
  iocoro::io_context ctx;
  iocoro::condition_event ev;

  constexpr int n_waiters = 256;
  constexpr int n_threads = 4;

  std::atomic<int> waiting{0};
  std::atomic<int> woke{0};
  std::atomic<int> timed_out{0};

  auto task = [&]() -> iocoro::awaitable<void> {
    auto ex = co_await iocoro::this_coro::executor;

    std::vector<iocoro::awaitable<void>> joins;
    joins.reserve(n_waiters);

    // NOTE: Avoid invoking coroutine lambdas on temporary closure objects.
    // Some toolchains can mis-handle capture lifetimes across suspension when the closure is a
    // prvalue (ASan can show this as stack-use-after-return).
    auto waiter_fn = [&]() -> iocoro::awaitable<void> {
      waiting.fetch_add(1, std::memory_order_release);
      auto r = co_await iocoro::with_timeout(ev.async_wait(), 500ms);
      if (r) {
        woke.fetch_add(1, std::memory_order_relaxed);
      } else {
        timed_out.fetch_add(1, std::memory_order_relaxed);
      }
      co_return;
    };

    for (int i = 0; i < n_waiters; ++i) {
      joins.push_back(iocoro::co_spawn(ex, waiter_fn(), iocoro::use_awaitable));
    }

    // Let all waiter coroutines start awaiting before notifying to reduce
    // "notify-before-wait" behavior (which can turn the wait into an immediate completion).
    auto const deadline = std::chrono::steady_clock::now() + 200ms;
    while (waiting.load(std::memory_order_acquire) < n_waiters &&
           std::chrono::steady_clock::now() < deadline) {
      co_await iocoro::co_sleep(1ms);
    }
    EXPECT_EQ(waiting.load(std::memory_order_acquire), n_waiters);

    std::vector<std::jthread> threads;
    threads.reserve(n_threads);
    for (int t = 0; t < n_threads; ++t) {
      threads.emplace_back([&](std::stop_token) {
        // Fixed number of notifications (no dependence on reactor-thread progress).
        // Extra notifications are accumulated as pending and are harmless.
        int const per_thread = ((n_waiters * 2) + n_threads - 1) / n_threads;
        for (int i = 0; i < per_thread; ++i) {
          ev.notify();
          std::this_thread::yield();
        }
      });
    }

    (void)co_await iocoro::when_all(std::move(joins));

    for (auto& th : threads) {
      th.join();
    }

    co_return;
  };

  auto r = iocoro::test::sync_wait(ctx, task());
  ASSERT_TRUE(r);
  EXPECT_EQ(timed_out.load(std::memory_order_relaxed), 0);
  EXPECT_EQ(woke.load(std::memory_order_relaxed), n_waiters);
}

TEST(condition_event_test, stress_stop_race_does_not_double_resume_or_hang) {
  iocoro::io_context ctx;

  constexpr int iters = 200;
  std::atomic<int> done{0};

  auto task = [&]() -> iocoro::awaitable<void> {
    auto ex = co_await iocoro::this_coro::executor;

    for (int i = 0; i < iters; ++i) {
      iocoro::condition_event ev;
      std::stop_source stop_src{};

      auto wait_fn = [&]() -> iocoro::awaitable<iocoro::result<void>> {
        co_return co_await ev.async_wait();
      };
      auto join = iocoro::co_spawn(ex, stop_src.get_token(), wait_fn(), iocoro::use_awaitable);

      // Mix immediate stop and "yield-then-stop" to exercise both races.
      if ((i % 2) == 1) {
        co_await iocoro::co_sleep(0ms);
      }

      stop_src.request_stop();

      auto r = co_await iocoro::with_timeout(std::move(join), 200ms);
      EXPECT_FALSE(static_cast<bool>(r));
      EXPECT_EQ(r.error(), iocoro::error::operation_aborted);

      done.fetch_add(1, std::memory_order_relaxed);
    }

    co_return;
  };

  auto r = iocoro::test::sync_wait(ctx, task());
  ASSERT_TRUE(r);
  EXPECT_EQ(done.load(std::memory_order_relaxed), iters);
}

}  // namespace iocoro::test

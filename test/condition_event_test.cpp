#include <gtest/gtest.h>

#include <iocoro/co_sleep.hpp>
#include <iocoro/iocoro.hpp>

#include "test_util.hpp"

#include <atomic>
#include <chrono>
#include <stop_token>
#include <thread>

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
    EXPECT_EQ(r.error(), iocoro::make_error_code(iocoro::error::operation_aborted));

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
  auto aborted_ec = iocoro::make_error_code(iocoro::error::operation_aborted);

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

}  // namespace iocoro::test

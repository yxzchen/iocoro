#include <gtest/gtest.h>

#include <iocoro/co_sleep.hpp>
#include <iocoro/co_spawn.hpp>
#include <iocoro/error.hpp>
#include <iocoro/io_context.hpp>
#include <iocoro/steady_timer.hpp>
#include <iocoro/with_timeout.hpp>
#include <iocoro/this_coro.hpp>

#include "test_util.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <optional>
#include <stop_token>
#include <thread>

using namespace std::chrono_literals;

// TEST(steady_timer_test, steady_timer_async_wait_resumes_on_fire) {
//   iocoro::io_context ctx;

//   auto r = iocoro::test::sync_wait(
//     ctx, [&]() -> iocoro::awaitable<iocoro::result<void>> {
//       iocoro::steady_timer t{ctx.get_executor(), std::chrono::milliseconds{1}};
//       co_return co_await t.async_wait(iocoro::use_awaitable);
//     }());

//   ASSERT_TRUE(r);
//   ASSERT_TRUE(*r);
// }

TEST(steady_timer_test, async_wait_completes_when_expired_in_past) {
  iocoro::io_context ctx;
  auto ex = ctx.get_executor();

  auto r = iocoro::test::sync_wait(ctx, [&]() -> iocoro::awaitable<iocoro::result<void>> {
    iocoro::steady_timer t{ex};
    t.expires_at(iocoro::steady_timer::clock::now() - 1ns);
    co_return co_await t.async_wait(iocoro::use_awaitable);
  }());

  ASSERT_TRUE(r);
  ASSERT_TRUE(*r);
}

TEST(steady_timer_test, expires_after_zero_completes) {
  iocoro::io_context ctx;
  auto ex = ctx.get_executor();

  auto r = iocoro::test::sync_wait(ctx, [&]() -> iocoro::awaitable<iocoro::result<void>> {
    iocoro::steady_timer t{ex};
    t.expires_after(0ms);
    co_return co_await t.async_wait(iocoro::use_awaitable);
  }());

  ASSERT_TRUE(r);
  ASSERT_TRUE(*r);
}

TEST(steady_timer_test, cancel_timer_prevents_execution) {
  iocoro::io_context ctx;
  auto ex = ctx.get_executor();

  iocoro::steady_timer t{ex, std::chrono::seconds{1}};
  std::optional<iocoro::expected<iocoro::result<void>, std::exception_ptr>> result;

  iocoro::co_spawn(
    ex,
    [&]() -> iocoro::awaitable<iocoro::result<void>> {
      co_return co_await t.async_wait(iocoro::use_awaitable);
    },
    [&](iocoro::expected<iocoro::result<void>, std::exception_ptr> r) { result = std::move(r); });

  ctx.run_one();
  t.cancel();
  ctx.run();
  ctx.restart();

  ASSERT_TRUE(result.has_value());
  ASSERT_TRUE(*result);
  ASSERT_FALSE(**result);
  EXPECT_EQ((**result).error(), iocoro::error::operation_aborted);
}

TEST(steady_timer_test, expires_after_while_waiting_aborts_previous_wait_and_new_wait_succeeds) {
  iocoro::io_context ctx;

  auto r = iocoro::test::sync_wait(ctx, [&]() -> iocoro::awaitable<void> {
    auto ex = co_await iocoro::this_coro::io_executor;
    iocoro::steady_timer t{ex};
    t.expires_after(24h);

    auto w1 = iocoro::co_spawn(
      ex,
      [&]() -> iocoro::awaitable<iocoro::result<void>> { co_return co_await t.async_wait(iocoro::use_awaitable); },
      iocoro::use_awaitable);

    co_await iocoro::co_sleep(1ms);

    t.expires_after(0ms);

    auto r1 = co_await std::move(w1);
    if (r1) {
      ADD_FAILURE() << "expected operation_aborted";
      co_return;
    }
    EXPECT_EQ(r1.error(), iocoro::make_error_code(iocoro::error::operation_aborted));

    auto r2 = co_await t.async_wait(iocoro::use_awaitable);
    if (!r2) {
      ADD_FAILURE() << "expected success";
      co_return;
    }
    co_return;
  }());

  ASSERT_TRUE(r);
}

TEST(steady_timer_test, second_async_wait_cancels_first) {
  iocoro::io_context ctx;

  auto r = iocoro::test::sync_wait(ctx, [&]() -> iocoro::awaitable<void> {
    auto ex = co_await iocoro::this_coro::io_executor;
    iocoro::steady_timer t{ex};

    t.expires_after(24h);
    auto w1 = iocoro::co_spawn(
      ex,
      [&]() -> iocoro::awaitable<iocoro::result<void>> { co_return co_await t.async_wait(iocoro::use_awaitable); },
      iocoro::use_awaitable);

    co_await iocoro::co_sleep(1ms);

    t.expires_after(0ms);
    auto r2 = co_await t.async_wait(iocoro::use_awaitable);
    if (!r2) {
      ADD_FAILURE() << "expected success";
      co_return;
    }

    auto r1 = co_await std::move(w1);
    if (r1) {
      ADD_FAILURE() << "expected operation_aborted";
      co_return;
    }
    EXPECT_EQ(r1.error(), iocoro::make_error_code(iocoro::error::operation_aborted));
    co_return;
  }());

  ASSERT_TRUE(r);
}

TEST(steady_timer_test, destroy_timer_aborts_waiter) {
  iocoro::io_context ctx;
  auto aborted = iocoro::make_error_code(iocoro::error::operation_aborted);

  auto r = iocoro::test::sync_wait(ctx, [&]() -> iocoro::awaitable<void> {
    auto ex = co_await iocoro::this_coro::io_executor;
    auto* t = new iocoro::steady_timer{ex};
    t->expires_after(24h);

    std::atomic<bool> started{false};
    std::jthread killer{[&](std::stop_token) {
      (void)iocoro::test::spin_wait_for([&] { return started.load(std::memory_order_acquire); }, 1s);
      std::this_thread::sleep_for(1ms);
      delete t;
    }};

    auto a = t->async_wait(iocoro::use_awaitable);
    started.store(true, std::memory_order_release);
    auto rr = co_await std::move(a);
    if (rr) {
      ADD_FAILURE() << "expected operation_aborted";
      co_return;
    }
    EXPECT_EQ(rr.error(), aborted);
    co_return;
  }());

  ASSERT_TRUE(r);
}

TEST(steady_timer_test, cancel_and_expires_from_foreign_thread_no_double_completion) {
  iocoro::io_context ctx;

  std::atomic<int> completed{0};
  std::atomic<int> aborted{0};
  std::atomic<bool> timed_out{false};

  auto task = [&]() -> iocoro::awaitable<void> {
    auto ioex = co_await iocoro::this_coro::io_executor;
    iocoro::steady_timer t{ioex};

    // Keep the foreign-thread racing active for the full duration of the awaits.
    // Without this, the worker may finish before any async_wait registers its cancellation
    // handle, leaving the timer scheduled far in the future and causing the test to hang.
    std::atomic<bool> done{false};
    std::jthread worker{[&](std::stop_token) {
      while (!done.load(std::memory_order_acquire)) {
        t.expires_after(0ms);
        t.cancel();
        std::this_thread::yield();
      }
    }};

    for (int i = 0; i < 50; ++i) {
      t.expires_after(24h);
      // Guard against hangs: if cancellation is missed due to scheduling races, fail fast
      // instead of stalling the whole test suite.
      auto rr = co_await iocoro::with_timeout(t.async_wait(iocoro::use_awaitable), 200ms);
      if (rr) {
        completed.fetch_add(1, std::memory_order_relaxed);
      } else {
        if (rr.error() == iocoro::make_error_code(iocoro::error::timed_out)) {
          timed_out.store(true, std::memory_order_relaxed);
          co_return;
        }
        EXPECT_EQ(rr.error(), iocoro::make_error_code(iocoro::error::operation_aborted));
        aborted.fetch_add(1, std::memory_order_relaxed);
      }
    }
    done.store(true, std::memory_order_release);
    co_return;
  };

  auto r = iocoro::test::sync_wait(ctx, task());
  ASSERT_TRUE(r);
  EXPECT_FALSE(timed_out.load(std::memory_order_relaxed));
  EXPECT_EQ(completed.load(std::memory_order_relaxed) + aborted.load(std::memory_order_relaxed), 50);
}

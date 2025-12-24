#include <gtest/gtest.h>

#include <iocoro/co_spawn.hpp>
#include <iocoro/error.hpp>
#include <iocoro/executor.hpp>
#include <iocoro/io_context.hpp>
#include <iocoro/src.hpp>
#include <iocoro/steady_timer.hpp>
#include <iocoro/timer_handle.hpp>
#include <iocoro/use_awaitable.hpp>

#include <atomic>
#include <chrono>

namespace {

TEST(timer_test, steady_timer_async_wait_resumes_on_fire) {
  using namespace std::chrono_literals;

  iocoro::io_context ctx;
  auto ex = ctx.get_executor();
  std::atomic<bool> done{false};
  std::atomic<bool> aborted{false};

  iocoro::steady_timer t{ex};
  t.expires_after(10ms);

  auto task = [&]() -> iocoro::awaitable<void> {
    auto ec = co_await t.async_wait(iocoro::use_awaitable);
    aborted.store(ec == iocoro::error::operation_aborted, std::memory_order_relaxed);
    done.store(true, std::memory_order_relaxed);
    co_return;
  };

  iocoro::co_spawn(ctx.get_executor(), task(), iocoro::detached);

  (void)ctx.run_for(200ms);
  EXPECT_TRUE(done.load(std::memory_order_relaxed));
  EXPECT_FALSE(aborted.load(std::memory_order_relaxed));
}

TEST(timer_test, steady_timer_async_wait_resumes_on_cancel) {
  using namespace std::chrono_literals;

  iocoro::io_context ctx;
  auto ex = ctx.get_executor();
  std::atomic<bool> done{false};
  std::atomic<bool> aborted{false};

  iocoro::steady_timer t{ex};
  t.expires_after(200ms);

  auto task = [&]() -> iocoro::awaitable<void> {
    auto ec = co_await t.async_wait(iocoro::use_awaitable);
    aborted.store(ec == iocoro::error::operation_aborted, std::memory_order_relaxed);
    done.store(true, std::memory_order_relaxed);
    co_return;
  };

  iocoro::co_spawn(ctx.get_executor(), task(), iocoro::detached);

  // Let the coroutine start and suspend on async_wait, then cancel it.
  (void)ctx.run_one();
  (void)t.cancel();

  (void)ctx.run_for(50ms);
  EXPECT_TRUE(done.load(std::memory_order_relaxed));
  EXPECT_TRUE(aborted.load(std::memory_order_relaxed));
}

}  // namespace

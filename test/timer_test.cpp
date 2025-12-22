#include <gtest/gtest.h>

#include <xz/io/co_spawn.hpp>
#include <xz/io/error.hpp>
#include <xz/io/executor.hpp>
#include <xz/io/io_context.hpp>
#include <xz/io/src.hpp>
#include <xz/io/steady_timer.hpp>
#include <xz/io/timer_handle.hpp>
#include <xz/io/use_awaitable.hpp>

#include <atomic>
#include <chrono>

namespace {

TEST(timer_test, steady_timer_async_wait_resumes_on_fire) {
  using namespace std::chrono_literals;

  xz::io::io_context ctx;
  auto ex = ctx.get_executor();
  std::atomic<bool> done{false};
  std::atomic<bool> aborted{false};

  xz::io::steady_timer t{ex};
  t.expires_after(10ms);

  auto task = [&]() -> xz::io::awaitable<void> {
    auto ec = co_await t.async_wait(xz::io::use_awaitable);
    aborted.store(ec == xz::io::make_error_code(xz::io::error::operation_aborted),
                  std::memory_order_relaxed);
    done.store(true, std::memory_order_relaxed);
    co_return;
  };

  xz::io::co_spawn(ctx.get_executor(), task(), xz::io::detached);

  (void)ctx.run_for(200ms);
  EXPECT_TRUE(done.load(std::memory_order_relaxed));
  EXPECT_FALSE(aborted.load(std::memory_order_relaxed));
}

TEST(timer_test, steady_timer_async_wait_resumes_on_cancel) {
  using namespace std::chrono_literals;

  xz::io::io_context ctx;
  auto ex = ctx.get_executor();
  std::atomic<bool> done{false};
  std::atomic<bool> aborted{false};

  xz::io::steady_timer t{ex};
  t.expires_after(200ms);

  auto task = [&]() -> xz::io::awaitable<void> {
    auto ec = co_await t.async_wait(xz::io::use_awaitable);
    aborted.store(ec == xz::io::make_error_code(xz::io::error::operation_aborted),
                  std::memory_order_relaxed);
    done.store(true, std::memory_order_relaxed);
    co_return;
  };

  xz::io::co_spawn(ctx.get_executor(), task(), xz::io::detached);

  // Let the coroutine start and suspend on async_wait, then cancel it.
  (void)ctx.run_one();
  (void)t.cancel();

  (void)ctx.run_for(50ms);
  EXPECT_TRUE(done.load(std::memory_order_relaxed));
  EXPECT_TRUE(aborted.load(std::memory_order_relaxed));
}

}  // namespace



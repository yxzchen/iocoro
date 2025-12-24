#include <gtest/gtest.h>

#include <iocoro/co_spawn.hpp>
#include <iocoro/error.hpp>
#include <iocoro/executor.hpp>
#include <iocoro/io_context.hpp>
#include <iocoro/src.hpp>
#include <iocoro/steady_timer.hpp>
#include <iocoro/timer_handle.hpp>
#include <iocoro/use_awaitable.hpp>

#include "test_util.hpp"

#include <chrono>

namespace {

TEST(timer_test, steady_timer_async_wait_resumes_on_fire) {
  using namespace std::chrono_literals;

  iocoro::io_context ctx;
  auto ex = ctx.get_executor();

  iocoro::steady_timer t{ex};
  t.expires_after(10ms);

  auto task = [&]() -> iocoro::awaitable<std::error_code> {
    co_return co_await t.async_wait(iocoro::use_awaitable);
  };

  auto ec = iocoro::sync_wait_for(ctx, 200ms, task());
  EXPECT_FALSE(ec) << ec.message();
  EXPECT_NE(ec, iocoro::error::operation_aborted);
}

TEST(timer_test, steady_timer_async_wait_resumes_on_cancel) {
  using namespace std::chrono_literals;

  iocoro::io_context ctx;
  auto ex = ctx.get_executor();

  iocoro::steady_timer t{ex};
  t.expires_after(200ms);

  auto task = [&]() -> iocoro::awaitable<std::error_code> {
    co_return co_await t.async_wait(iocoro::use_awaitable);
  };

  auto wait = iocoro::co_spawn(ex, task(), iocoro::use_awaitable);

  // Let the coroutine start and suspend on async_wait, then cancel it.
  (void)ctx.run_one();
  (void)t.cancel();

  auto ec = iocoro::sync_wait_for(ctx, 50ms, std::move(wait));
  EXPECT_EQ(ec, iocoro::error::operation_aborted);
}

}  // namespace

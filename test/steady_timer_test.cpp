#include <gtest/gtest.h>

#include <iocoro/co_spawn.hpp>
#include <iocoro/error.hpp>
#include <iocoro/io_context.hpp>
#include <iocoro/steady_timer.hpp>

#include "test_util.hpp"

#include <chrono>
#include <optional>

TEST(steady_timer_test, steady_timer_async_wait_resumes_on_fire) {
  iocoro::io_context ctx;

  auto r = iocoro::test::sync_wait(
    ctx, [&]() -> iocoro::awaitable<std::error_code> {
      iocoro::steady_timer t{ctx.get_executor(), std::chrono::milliseconds{1}};
      co_return co_await t.async_wait(iocoro::use_awaitable);
    }());

  ASSERT_TRUE(r);
  EXPECT_EQ(*r, std::error_code{});
}

TEST(steady_timer_test, cancel_timer_prevents_execution) {
  iocoro::io_context ctx;
  auto ex = ctx.get_executor();

  iocoro::steady_timer t{ex, std::chrono::seconds{1}};
  std::optional<iocoro::expected<std::error_code, std::exception_ptr>> result;

  iocoro::co_spawn(
    ex,
    [&]() -> iocoro::awaitable<std::error_code> {
      co_return co_await t.async_wait(iocoro::use_awaitable);
    },
    [&](iocoro::expected<std::error_code, std::exception_ptr> r) { result = std::move(r); });

  ctx.run_one();
  t.cancel();
  ctx.run();
  ctx.restart();

  ASSERT_TRUE(result.has_value());
  ASSERT_TRUE(*result);
  EXPECT_EQ(**result, iocoro::error::operation_aborted);
}

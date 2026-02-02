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
    ctx, [&]() -> iocoro::awaitable<iocoro::result<void>> {
      iocoro::steady_timer t{ctx.get_executor(), std::chrono::milliseconds{1}};
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

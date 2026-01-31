#include <gtest/gtest.h>

#include <iocoro/co_sleep.hpp>
#include <iocoro/io_context.hpp>

#include "test_util.hpp"

#include <chrono>

TEST(co_sleep_test, co_sleep_resumes_via_timer_and_executor) {
  iocoro::io_context ctx;

  auto r = iocoro::test::sync_wait(
    ctx, [&]() -> iocoro::awaitable<void> {
      co_await iocoro::co_sleep(ctx.get_executor(), std::chrono::milliseconds{1});
    }());

  ASSERT_TRUE(r);
}

TEST(co_sleep_test, co_sleep_uses_current_executor) {
  iocoro::io_context ctx;

  auto r = iocoro::test::sync_wait(
    ctx, [&]() -> iocoro::awaitable<void> {
      co_await iocoro::co_sleep(std::chrono::milliseconds{1});
    }());

  ASSERT_TRUE(r);
}

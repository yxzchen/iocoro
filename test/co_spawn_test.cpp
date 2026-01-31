#include <gtest/gtest.h>

#include <iocoro/co_spawn.hpp>
#include <iocoro/io_context.hpp>

#include "test_util.hpp"

#include <atomic>
#include <stdexcept>

TEST(co_spawn_test, co_spawn_factory_detached_runs) {
  iocoro::io_context ctx;
  auto ex = ctx.get_executor();

  std::atomic<int> count{0};
  iocoro::co_spawn(
    ex,
    [&]() -> iocoro::awaitable<void> {
      ++count;
      co_return;
    },
    iocoro::detached);

  ctx.run();
  EXPECT_EQ(count.load(), 1);
}

TEST(co_spawn_test, co_spawn_use_awaitable_returns_value) {
  iocoro::io_context ctx;

  auto r = iocoro::test::sync_wait(
    ctx, iocoro::co_spawn(
            ctx.get_executor(),
            []() -> iocoro::awaitable<int> {
              co_return 42;
            },
            iocoro::use_awaitable));

  ASSERT_TRUE(r);
  EXPECT_EQ(*r, 42);
}

TEST(co_spawn_test, co_spawn_use_awaitable_rethrows_exception) {
  iocoro::io_context ctx;

  auto r = iocoro::test::sync_wait(
    ctx, iocoro::co_spawn(
            ctx.get_executor(),
            []() -> iocoro::awaitable<int> {
              throw std::runtime_error{"boom"};
              co_return 0;
            },
            iocoro::use_awaitable));

  ASSERT_FALSE(r);
  ASSERT_TRUE(r.error());
}

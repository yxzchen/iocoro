#include <gtest/gtest.h>

#include <iocoro/co_spawn.hpp>
#include <iocoro/completion_token.hpp>
#include <iocoro/error.hpp>
#include <iocoro/any_io_executor.hpp>
#include <iocoro/io_context.hpp>
#include <iocoro/steady_timer.hpp>

#include "test_util.hpp"

#include <chrono>

namespace {

// TEST(steady_timer_test, steady_timer_async_wait_resumes_on_fire) {
//   using namespace std::chrono_literals;

//   iocoro::io_context ctx;
//   auto ex = ctx.get_executor();

//   iocoro::steady_timer t{ex};
//   t.expires_after(10ms);

//   auto task = [&]() -> iocoro::awaitable<std::error_code> {
//     return t.async_wait(iocoro::use_awaitable);
//   };

//   auto ec = iocoro::sync_wait_for(ctx, 200ms, task());
//   EXPECT_FALSE(ec) << ec.message();
//   EXPECT_NE(ec, iocoro::error::operation_aborted);
// }

TEST(steady_timer_test, steady_timer_async_wait_resumes_on_fire) {
  using namespace std::chrono_literals;

  iocoro::io_context ctx;
  auto ex = ctx.get_executor();

  iocoro::steady_timer t{ex};
  t.expires_after(10ms);

  auto task = [&]() -> iocoro::awaitable<std::error_code> {
    return t.async_wait(iocoro::use_awaitable);
  };

  auto ec = iocoro::sync_wait_for(ctx, 200ms, task());
  EXPECT_FALSE(ec) << ec.message();
}

}  // namespace

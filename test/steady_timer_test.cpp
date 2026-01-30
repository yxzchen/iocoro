#include <gtest/gtest.h>

#include <iocoro/co_spawn.hpp>
#include <iocoro/completion_token.hpp>
#include <stop_token>
#include <iocoro/error.hpp>
#include <iocoro/any_io_executor.hpp>
#include <iocoro/io_context.hpp>
#include <iocoro/steady_timer.hpp>
#include <iocoro/this_coro.hpp>

#include "test_util.hpp"

#include <chrono>
#include <thread>

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

TEST(steady_timer_test, steady_timer_async_wait_resumes_on_cancel) {
  using namespace std::chrono_literals;

  iocoro::io_context ctx;
  auto ex = ctx.get_executor();

  iocoro::steady_timer t{ex};
  t.expires_after(200ms);

  auto task = [&]() -> iocoro::awaitable<std::error_code> {
    return t.async_wait(iocoro::use_awaitable);
  };

  auto wait = iocoro::co_spawn(ex, task(), iocoro::use_awaitable);

  // Let the coroutine start and suspend on async_wait, then cancel it.
  (void)ctx.run_one();
  (void)t.cancel();

  auto ec = iocoro::sync_wait_for(ctx, 50ms, std::move(wait));
  EXPECT_EQ(ec, iocoro::error::operation_aborted);
}

TEST(steady_timer_test, stop_token_does_not_hang_under_race) {
  using namespace std::chrono_literals;

  iocoro::io_context ctx;
  auto ex = ctx.get_executor();

  auto ec = iocoro::sync_wait_for(ctx, 5s, [&]() -> iocoro::awaitable<std::error_code> {
    for (int i = 0; i < 200; ++i) {
      iocoro::steady_timer t{ex};
      t.expires_after(5s);

      std::stop_source src{};
      auto tok = src.get_token();

      std::thread th([&] { src.request_stop(); });
      auto scope = co_await iocoro::this_coro::set_stop_token(tok);
      auto out = co_await t.async_wait(iocoro::use_awaitable);
      scope.reset();
      th.join();

      if (out != iocoro::error::operation_aborted) {
        co_return out;
      }
    }
    co_return iocoro::make_error_code(iocoro::error::operation_aborted);
  }());

  EXPECT_EQ(ec, iocoro::error::operation_aborted);
}

}  // namespace

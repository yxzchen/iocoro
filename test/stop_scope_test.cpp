#include <iocoro/iocoro.hpp>
#include "test_util.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <thread>

namespace iocoro::test {

using namespace std::chrono_literals;

TEST(stop_scope_test, reset_produces_fresh_token) {
  iocoro::stop_scope scope{};
  auto t0 = scope.get_token();
  EXPECT_TRUE(t0.stop_possible());
  EXPECT_FALSE(t0.stop_requested());

  scope.request_stop();
  EXPECT_TRUE(t0.stop_requested());

  scope.reset();
  auto t1 = scope.get_token();
  EXPECT_TRUE(t1.stop_possible());
  EXPECT_FALSE(t1.stop_requested());
}

TEST(stop_scope_test, bind_stop_token_propagates_stop_into_awaitable) {
  iocoro::io_context ctx;
  iocoro::stop_scope scope{};

  auto aborted_ec = iocoro::make_error_code(iocoro::error::operation_aborted);

  auto task = iocoro::bind_stop_token(scope.get_token(), [&]() -> iocoro::awaitable<void> {
    iocoro::notify_event ev{};
    auto w = co_await ev.async_wait(iocoro::use_awaitable);
    if (w) {
      ADD_FAILURE() << "expected operation_aborted, got success";
    } else {
      EXPECT_EQ(w.error(), aborted_ec);
    }
    co_return;
  }());

  std::jthread stopper{[&]() {
    std::this_thread::sleep_for(1ms);
    scope.request_stop();
  }};

  auto rr = iocoro::test::sync_wait(
    ctx, iocoro::co_spawn(ctx.get_executor(), std::move(task), iocoro::use_awaitable));
  ASSERT_TRUE(rr);
}

}  // namespace iocoro::test


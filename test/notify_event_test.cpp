#include <iocoro/iocoro.hpp>
#include "test_util.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <stop_token>
#include <thread>

namespace iocoro::test {

using namespace std::chrono_literals;

TEST(notify_event_test, notify_before_wait_is_consumed_immediately) {
  iocoro::io_context ctx;

  auto r = iocoro::test::sync_wait(ctx, [&]() -> iocoro::awaitable<void> {
    iocoro::notify_event ev{};
    ev.notify_one();

    auto w = co_await ev.async_wait(iocoro::use_awaitable);
    EXPECT_TRUE(w);
    co_return;
  }());

  ASSERT_TRUE(r);
}

TEST(notify_event_test, wait_then_notify_resumes_waiter) {
  iocoro::io_context ctx;

  auto r = iocoro::test::sync_wait(ctx, [&]() -> iocoro::awaitable<void> {
    auto ex = co_await iocoro::this_coro::io_executor;
    iocoro::notify_event ev{};

    // Notify from the executor to avoid races with io_context lifetime.
    ex.post([&ev]() noexcept { ev.notify_one(); });

    auto w = co_await ev.async_wait(iocoro::use_awaitable);
    EXPECT_TRUE(w);
    co_return;
  }());

  ASSERT_TRUE(r);
}

TEST(notify_event_test, stop_while_waiting_resumes_with_operation_aborted) {
  iocoro::io_context ctx;
  std::stop_source stop_src{};

  auto aborted_ec = iocoro::make_error_code(iocoro::error::operation_aborted);

  auto task = [&]() -> iocoro::awaitable<void> {
    iocoro::notify_event ev{};
    auto w = co_await ev.async_wait(iocoro::use_awaitable);
    if (w) {
      ADD_FAILURE() << "expected operation_aborted, got success";
    } else {
      EXPECT_EQ(w.error(), aborted_ec);
    }
    co_return;
  };

  std::jthread stopper{[&]() {
    std::this_thread::sleep_for(1ms);
    stop_src.request_stop();
  }};

  auto r = iocoro::test::sync_wait(
    ctx, iocoro::co_spawn(ctx.get_executor(), stop_src.get_token(), task(), iocoro::use_awaitable));
  ASSERT_TRUE(r);
}

}  // namespace iocoro::test


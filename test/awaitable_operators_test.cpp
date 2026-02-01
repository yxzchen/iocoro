#include <iocoro/iocoro.hpp>
#include "test_util.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <optional>
#include <system_error>
#include <variant>

namespace iocoro::test {

using namespace std::chrono_literals;

auto wait_timer_value(iocoro::steady_timer& timer,
                      std::shared_ptr<std::optional<std::error_code>> out,
                      int value) -> iocoro::awaitable<int> {
  auto ec = co_await timer.async_wait(iocoro::use_awaitable);
  *out = ec;
  co_return value;
}

auto wait_timer_ec(iocoro::steady_timer& timer,
                   std::shared_ptr<std::optional<std::error_code>> out)
  -> iocoro::awaitable<std::error_code> {
  auto ec = co_await timer.async_wait(iocoro::use_awaitable);
  *out = ec;
  co_return ec;
}

TEST(awaitable_operators, timer_race_cancels_loser) {
  iocoro::io_context ctx;

  auto fast_ec = std::make_shared<std::optional<std::error_code>>();
  auto slow_ec = std::make_shared<std::optional<std::error_code>>();

  auto task = [&]() -> iocoro::awaitable<void> {
    auto ex = co_await iocoro::this_coro::io_executor;

    iocoro::steady_timer fast_timer(ex);
    fast_timer.expires_after(5ms);

    iocoro::steady_timer slow_timer(ex);
    slow_timer.expires_after(50ms);

    auto [index, result] =
      co_await (wait_timer_value(fast_timer, fast_ec, 7) || wait_timer_ec(slow_timer, slow_ec));

    EXPECT_EQ(index, 0U);
    EXPECT_EQ(std::get<int>(result), 7);
  };

  auto r = iocoro::test::sync_wait(ctx, task());
  ASSERT_TRUE(r);

  auto aborted = iocoro::make_error_code(iocoro::error::operation_aborted);
  ASSERT_TRUE(fast_ec->has_value());
  EXPECT_EQ(fast_ec->value(), std::error_code{});
  ASSERT_TRUE(slow_ec->has_value());
  EXPECT_EQ(slow_ec->value(), aborted);
}

}  // namespace iocoro::test

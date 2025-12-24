#include <gtest/gtest.h>

#include <iocoro/awaitable.hpp>
#include <iocoro/co_sleep.hpp>
#include <iocoro/co_spawn.hpp>
#include <iocoro/io_context.hpp>
#include <iocoro/src.hpp>

#include <chrono>

#include "test_util.hpp"

namespace {

TEST(co_sleep_test, co_sleep_resumes_via_timer_and_executor) {
  using namespace std::chrono_literals;

  iocoro::io_context ctx;
  auto done = iocoro::sync_wait_for(
    ctx, 200ms, [&]() -> iocoro::awaitable<bool> {
      co_await iocoro::co_sleep(10ms);
      co_return true;
    }());

  EXPECT_TRUE(done);
}

}  // namespace

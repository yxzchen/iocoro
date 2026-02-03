#include <gtest/gtest.h>

#include <iocoro/co_sleep.hpp>
#include <iocoro/iocoro.hpp>

#include "test_util.hpp"

#include <atomic>
#include <chrono>
#include <stop_token>
#include <thread>

namespace iocoro::test {

using namespace std::chrono_literals;

TEST(this_coro_stop_token_test, yields_token_and_observes_stop_after_cache) {
  iocoro::io_context ctx;
  std::stop_source stop_src{};

  std::atomic<bool> saw_stop{false};

  auto task = [&]() -> iocoro::awaitable<void> {
    auto tok = co_await iocoro::this_coro::stop_token;
    EXPECT_TRUE(tok.stop_possible());

    // Allow the test thread to request stop.
    co_await iocoro::co_sleep(5ms);
    saw_stop.store(tok.stop_requested());
    co_return;
  };

  std::jthread stopper{[&]() {
    std::this_thread::sleep_for(1ms);
    stop_src.request_stop();
  }};

  auto r = iocoro::test::sync_wait(
    ctx, iocoro::co_spawn(ctx.get_executor(), stop_src.get_token(), task(), iocoro::use_awaitable));
  ASSERT_TRUE(r);
  EXPECT_TRUE(saw_stop.load());
}

}  // namespace iocoro::test

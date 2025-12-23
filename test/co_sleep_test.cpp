#include <gtest/gtest.h>

#include <iocoro/awaitable.hpp>
#include <iocoro/co_sleep.hpp>
#include <iocoro/co_spawn.hpp>
#include <iocoro/io_context.hpp>
#include <iocoro/src.hpp>

#include <atomic>
#include <chrono>

namespace {

TEST(co_sleep_test, co_sleep_resumes_via_timer_and_executor) {
  using namespace std::chrono_literals;

  iocoro::io_context ctx;
  std::atomic<bool> done{false};

  auto task = [&]() -> iocoro::awaitable<void> {
    co_await iocoro::co_sleep(10ms);
    done.store(true, std::memory_order_relaxed);
  };

  iocoro::co_spawn(ctx.get_executor(), task(), iocoro::detached);

  (void)ctx.run_for(200ms);
  EXPECT_TRUE(done.load(std::memory_order_relaxed));
}

}  // namespace

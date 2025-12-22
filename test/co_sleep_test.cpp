#include <gtest/gtest.h>

#include <xz/io/awaitable.hpp>
#include <xz/io/co_sleep.hpp>
#include <xz/io/co_spawn.hpp>
#include <xz/io/io_context.hpp>
#include <xz/io/src.hpp>

#include <atomic>
#include <chrono>

namespace {

TEST(co_sleep_test, co_sleep_resumes_via_timer_and_executor) {
  using namespace std::chrono_literals;

  xz::io::io_context ctx;
  std::atomic<bool> done{false};

  auto task = [&]() -> xz::io::awaitable<void> {
    co_await xz::io::co_sleep(10ms);
    done.store(true, std::memory_order_relaxed);
  };

  xz::io::co_spawn(ctx.get_executor(), task(), xz::io::detached);

  (void)ctx.run_for(200ms);
  EXPECT_TRUE(done.load(std::memory_order_relaxed));
}

}  // namespace



#include <gtest/gtest.h>

#include <iocoro/co_spawn.hpp>
#include <iocoro/io_context.hpp>
#include <iocoro/this_coro.hpp>
#include <iocoro/thread_pool.hpp>

#include <atomic>
#include <condition_variable>
#include <mutex>

TEST(switch_to_test, switches_executor_and_thread_pool_thread) {
  iocoro::io_context ctx;
  iocoro::thread_pool pool{1};

  auto ex1 = ctx.get_executor();
  auto ex2 = pool.get_executor();

  std::mutex m;
  std::condition_variable cv;
  std::atomic<bool> on_pool{false};
  std::atomic<bool> done{false};

  iocoro::co_spawn(
    ex1,
    [&]() -> iocoro::awaitable<void> {
      auto before = co_await iocoro::this_coro::executor;
      EXPECT_EQ(before, iocoro::any_executor{ex1});

      co_await iocoro::this_coro::switch_to(iocoro::any_executor{ex2});

      auto after = co_await iocoro::this_coro::executor;
      if (after == iocoro::any_executor{ex2}) {
        on_pool.store(true);
      }
      co_return;
    },
    [&](iocoro::expected<void, std::exception_ptr> r) {
      (void)r;
      std::scoped_lock lk{m};
      done.store(true);
      cv.notify_all();
    });

  ctx.run();

  std::unique_lock lk{m};
  cv.wait(lk, [&] { return done.load(); });
  EXPECT_TRUE(on_pool.load());
}

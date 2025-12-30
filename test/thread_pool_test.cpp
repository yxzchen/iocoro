#include <gtest/gtest.h>

#include <iocoro/iocoro.hpp>
#include <iocoro/impl.hpp>
#include <iocoro/thread_pool_executor.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <thread>
#include <unordered_set>

namespace {

using namespace std::chrono_literals;

TEST(thread_pool, post_runs_on_multiple_threads) {
  iocoro::thread_pool pool{4};
  auto ex = pool.get_executor();

  std::mutex m;
  std::condition_variable cv;
  std::unordered_set<std::thread::id> threads;

  std::atomic<int> remaining{200};

  for (int i = 0; i < 200; ++i) {
    ex.post([&] {
      {
        std::scoped_lock lk{m};
        threads.insert(std::this_thread::get_id());
      }
      if (remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        cv.notify_one();
      }
    });
  }

  {
    std::unique_lock lk{m};
    cv.wait_for(lk, 2s, [&] { return remaining.load(std::memory_order_acquire) == 0; });
  }

  EXPECT_EQ(remaining.load(std::memory_order_acquire), 0);

  {
    std::scoped_lock lk{m};
    EXPECT_GT(threads.size(), 1u);
  }
}

TEST(thread_pool, co_spawn_accepts_thread_pool_executor) {
  iocoro::thread_pool pool{2};
  auto ex = pool.get_executor();

  auto done = std::make_shared<std::promise<void>>();
  auto fut = done->get_future();

  std::atomic<bool> saw_executor{false};

  iocoro::co_spawn(
    ex,
    [done, &saw_executor]() -> iocoro::awaitable<void> {
      auto current = co_await iocoro::this_coro::executor;
      if (current) {
        saw_executor.store(true, std::memory_order_release);
      }
      done->set_value();
      co_return;
    },
    iocoro::detached);

  EXPECT_EQ(fut.wait_for(2s), std::future_status::ready);
  fut.get();
  EXPECT_TRUE(saw_executor.load(std::memory_order_acquire));
}

}  // namespace



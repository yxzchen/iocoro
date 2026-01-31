#include <gtest/gtest.h>

#include <iocoro/thread_pool.hpp>

#include <array>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <stdexcept>

TEST(thread_pool_test, size_returns_thread_count) {
  iocoro::thread_pool pool{2};
  EXPECT_EQ(pool.size(), 2U);
}

TEST(thread_pool_test, executes_large_number_of_tasks) {
  iocoro::thread_pool pool{2};
  auto ex = pool.get_executor();

  std::mutex m;
  std::condition_variable cv;
  std::atomic<int> count{0};
  constexpr int total = 100;

  for (int i = 0; i < total; ++i) {
    ex.post([&] {
      auto v = count.fetch_add(1) + 1;
      if (v == total) {
        std::scoped_lock lk{m};
        cv.notify_all();
      }
    });
  }

  std::unique_lock lk{m};
  cv.wait(lk, [&] { return count.load() == total; });
}

TEST(thread_pool_test, exception_handler_is_called) {
  iocoro::thread_pool pool{1};
  auto ex = pool.get_executor();

  std::mutex m;
  std::condition_variable cv;
  std::atomic<bool> called{false};

  pool.set_exception_handler([&](std::exception_ptr ep) {
    if (ep) {
      called.store(true);
    }
    std::scoped_lock lk{m};
    cv.notify_all();
  });

  ex.post([] { throw std::runtime_error{"boom"}; });

  std::unique_lock lk{m};
  cv.wait(lk, [&] { return called.load(); });
  EXPECT_TRUE(called.load());
}

TEST(thread_pool_test, stop_and_join_are_idempotent) {
  iocoro::thread_pool pool{2};

  pool.stop();
  pool.stop();
  pool.join();
  pool.join();
}

TEST(thread_pool_test, dispatch_runs_inline_on_worker_thread) {
  iocoro::thread_pool pool{1};
  auto ex = pool.get_executor();

  std::mutex m;
  std::condition_variable cv;
  std::array<int, 3> order{};
  std::atomic<int> index{0};
  std::atomic<bool> done{false};

  ex.post([&] {
    order[static_cast<std::size_t>(index++)] = 1;
    ex.dispatch([&] { order[static_cast<std::size_t>(index++)] = 2; });
    order[static_cast<std::size_t>(index++)] = 3;

    std::scoped_lock lk{m};
    done.store(true);
    cv.notify_all();
  });

  std::unique_lock lk{m};
  cv.wait(lk, [&] { return done.load(); });
  EXPECT_EQ(order[0], 1);
  EXPECT_EQ(order[1], 2);
  EXPECT_EQ(order[2], 3);
}

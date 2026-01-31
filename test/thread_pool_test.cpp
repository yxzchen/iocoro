#include <gtest/gtest.h>

#include <iocoro/thread_pool.hpp>

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

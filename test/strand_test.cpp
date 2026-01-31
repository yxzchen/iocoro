#include <gtest/gtest.h>

#include <iocoro/strand.hpp>
#include <iocoro/thread_pool.hpp>

#include <atomic>
#include <array>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

TEST(strand_test, tasks_on_same_strand_never_run_concurrently) {
  iocoro::thread_pool pool{4};
  auto s = iocoro::make_strand(pool.get_executor());

  std::atomic<int> in_flight{0};
  std::atomic<int> max_in_flight{0};
  std::atomic<int> done{0};
  std::mutex m;
  std::condition_variable cv;
  constexpr int total = 20;

  for (int i = 0; i < total; ++i) {
    s.post([&] {
      auto cur = in_flight.fetch_add(1) + 1;
      int expected = max_in_flight.load();
      while (cur > expected) {
        if (max_in_flight.compare_exchange_weak(expected, cur)) {
          break;
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds{1});
      in_flight.fetch_sub(1);
      auto v = done.fetch_add(1) + 1;
      if (v == total) {
        std::scoped_lock lk{m};
        cv.notify_all();
      }
    });
  }

  std::unique_lock lk{m};
  cv.wait(lk, [&] { return done.load() == total; });
  EXPECT_EQ(max_in_flight.load(), 1);
}

TEST(strand_test, different_strands_do_not_serialize_each_other) {
  iocoro::thread_pool pool{4};
  auto s1 = iocoro::make_strand(pool.get_executor());
  auto s2 = iocoro::make_strand(pool.get_executor());

  std::atomic<int> in_flight{0};
  std::atomic<int> max_in_flight{0};
  std::atomic<int> done{0};
  std::mutex m;
  std::condition_variable cv;
  constexpr int total = 20;

  auto post_task = [&](iocoro::strand_executor& s) {
    s.post([&] {
      auto cur = in_flight.fetch_add(1) + 1;
      int expected = max_in_flight.load();
      while (cur > expected) {
        if (max_in_flight.compare_exchange_weak(expected, cur)) {
          break;
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds{2});
      in_flight.fetch_sub(1);
      auto v = done.fetch_add(1) + 1;
      if (v == total) {
        std::scoped_lock lk{m};
        cv.notify_all();
      }
    });
  };

  for (int i = 0; i < total / 2; ++i) {
    post_task(s1);
    post_task(s2);
  }

  std::unique_lock lk{m};
  cv.wait(lk, [&] { return done.load() == total; });
  EXPECT_GE(max_in_flight.load(), 2);
}

TEST(strand_test, dispatch_runs_inline_on_same_strand) {
  iocoro::thread_pool pool{2};
  auto s = iocoro::make_strand(pool.get_executor());

  std::mutex m;
  std::condition_variable cv;
  std::array<int, 3> order{};
  std::atomic<int> index{0};
  std::atomic<bool> done{false};

  s.post([&] {
    order[static_cast<std::size_t>(index++)] = 1;
    s.dispatch([&] { order[static_cast<std::size_t>(index++)] = 2; });
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

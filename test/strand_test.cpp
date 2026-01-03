#include <gtest/gtest.h>

#include <iocoro/iocoro.hpp>

#include <atomic>
#include <chrono>
#include <future>
#include <thread>

namespace {

using namespace std::chrono_literals;

static void update_max(std::atomic<int>& max_v, int v) {
  int cur = max_v.load(std::memory_order_relaxed);
  while (cur < v) {
    if (max_v.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {
      return;
    }
  }
}

TEST(strand_test, tasks_on_same_strand_never_run_concurrently) {
  iocoro::thread_pool pool{4};
  auto base = pool.get_executor();
  auto s = iocoro::make_strand(base);

  std::atomic<int> in_flight{0};
  std::atomic<int> max_in_flight{0};

  std::promise<void> done;
  auto fut = done.get_future();

  constexpr int num_tasks = 2000;
  for (int i = 0; i < num_tasks; ++i) {
    s.post([&, i] {
      int now = in_flight.fetch_add(1, std::memory_order_acq_rel) + 1;
      update_max(max_in_flight, now);

      // Make overlap likely if strand is broken.
      std::this_thread::sleep_for(50us);

      in_flight.fetch_sub(1, std::memory_order_acq_rel);

      if (i == num_tasks - 1) {
        done.set_value();
      }
    });
  }

  EXPECT_EQ(fut.wait_for(5s), std::future_status::ready);
  EXPECT_EQ(max_in_flight.load(std::memory_order_acquire), 1);
}

TEST(strand_test, dispatch_runs_inline_when_already_on_strand) {
  iocoro::thread_pool pool{2};
  auto s = iocoro::make_strand(pool.get_executor());

  std::promise<void> done;
  auto fut = done.get_future();

  std::thread::id outer_tid{};
  std::thread::id inner_tid{};

  s.post([&] {
    outer_tid = std::this_thread::get_id();
    s.dispatch([&] {
      inner_tid = std::this_thread::get_id();
      done.set_value();
    });
  });

  EXPECT_EQ(fut.wait_for(2s), std::future_status::ready);
  EXPECT_EQ(outer_tid, inner_tid);
}

TEST(strand_test, different_strands_do_not_serialize_each_other) {
  iocoro::thread_pool pool{4};
  auto base = pool.get_executor();

  auto s1 = iocoro::make_strand(base);
  auto s2 = iocoro::make_strand(base);

  std::atomic<int> in_flight{0};
  std::atomic<int> max_in_flight{0};

  std::promise<void> done;
  auto fut = done.get_future();

  // Long tasks that should overlap if the underlying executor has multiple threads.
  s1.post([&] {
    int now = in_flight.fetch_add(1, std::memory_order_acq_rel) + 1;
    update_max(max_in_flight, now);
    std::this_thread::sleep_for(200ms);
    in_flight.fetch_sub(1, std::memory_order_acq_rel);
  });

  s2.post([&] {
    int now = in_flight.fetch_add(1, std::memory_order_acq_rel) + 1;
    update_max(max_in_flight, now);
    std::this_thread::sleep_for(200ms);
    in_flight.fetch_sub(1, std::memory_order_acq_rel);
    done.set_value();
  });

  EXPECT_EQ(fut.wait_for(3s), std::future_status::ready);
  EXPECT_GE(max_in_flight.load(std::memory_order_acquire), 2);
}

}  // namespace



#include <gtest/gtest.h>

#include <iocoro/io_context.hpp>
#include <iocoro/thread_pool.hpp>
#include <iocoro/work_guard.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <thread>

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

TEST(thread_pool_test, dispatch_inline_exception_propagates_to_caller) {
  iocoro::thread_pool pool{1};
  auto ex = pool.get_executor();

  std::mutex m;
  std::condition_variable cv;
  std::atomic<bool> caught{false};
  std::atomic<bool> done{false};

  ex.post([&] {
    try {
      ex.dispatch([] { throw std::runtime_error{"boom"}; });
    } catch (std::runtime_error const&) {
      caught.store(true, std::memory_order_release);
    }

    std::scoped_lock lk{m};
    done.store(true, std::memory_order_release);
    cv.notify_all();
  });

  std::unique_lock lk{m};
  cv.wait(lk, [&] { return done.load(std::memory_order_acquire); });
  EXPECT_TRUE(caught.load(std::memory_order_acquire));
}

TEST(thread_pool_test, work_guard_keeps_context_alive_until_reset) {
  iocoro::io_context ctx;
  auto ex = ctx.get_executor();

  std::atomic<bool> ran{false};

  auto wg = iocoro::make_work_guard(ctx);

  std::jthread runner{[&] { (void)ctx.run(); }};

  // Give runner a moment to start; it should not exit because of work_guard.
  std::this_thread::sleep_for(std::chrono::milliseconds{2});

  ex.post([&] {
    ran.store(true, std::memory_order_release);
    wg.reset();
  });

  runner.join();
  ctx.restart();

  EXPECT_TRUE(ran.load(std::memory_order_acquire));
}

TEST(thread_pool_test, work_guard_reset_allows_run_to_return_when_no_work) {
  iocoro::io_context ctx;

  auto wg = iocoro::make_work_guard(ctx);
  wg.reset();

  auto n = ctx.run_for(std::chrono::milliseconds{1});
  EXPECT_EQ(n, 0U);
}

TEST(thread_pool_test, multiple_work_guards_reference_counting) {
  iocoro::io_context ctx;
  auto ex = ctx.get_executor();

  std::atomic<int> ran{0};

  auto g1 = iocoro::make_work_guard(ctx);
  auto g2 = iocoro::make_work_guard(ctx);

  std::jthread runner{[&] { (void)ctx.run(); }};

  ex.post([&] { ran.fetch_add(1, std::memory_order_relaxed); });
  std::this_thread::sleep_for(std::chrono::milliseconds{1});

  g1.reset();
  // still held by g2, loop should keep running
  ex.post([&] { ran.fetch_add(1, std::memory_order_relaxed); });
  std::this_thread::sleep_for(std::chrono::milliseconds{1});

  ex.post([&] {
    ran.fetch_add(1, std::memory_order_relaxed);
    g2.reset();
  });

  runner.join();
  ctx.restart();

  EXPECT_GE(ran.load(std::memory_order_relaxed), 2);
}

TEST(thread_pool_test, executor_stopped_reflects_pool_state) {
  iocoro::thread_pool pool{1};
  auto ex = pool.get_executor();

  EXPECT_FALSE(ex.stopped());
  pool.stop();
  EXPECT_TRUE(ex.stopped());

  pool.join();
  EXPECT_TRUE(ex.stopped());
}

TEST(thread_pool_test, post_after_stop_is_dropped) {
  iocoro::thread_pool pool{1};
  auto ex = pool.get_executor();

  pool.stop();

  std::atomic<int> ran{0};
  for (int i = 0; i < 1000; ++i) {
    ex.post([&] { ran.fetch_add(1, std::memory_order_relaxed); });
  }

  std::this_thread::sleep_for(std::chrono::milliseconds{10});
  EXPECT_EQ(ran.load(std::memory_order_relaxed), 0);

  pool.join();
}

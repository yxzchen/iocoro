#include <gtest/gtest.h>

#include <iocoro/detail/io_context_impl.hpp>
#include <iocoro/detail/reactor_types.hpp>

#include <atomic>
#include <chrono>

TEST(io_context_impl_test, post_and_run_executes_operations) {
  iocoro::detail::io_context_impl ctx;

  std::atomic<int> count{0};
  ctx.post([&] { ++count; });
  ctx.post([&] { ++count; });

  ctx.run();
  EXPECT_EQ(count.load(), 2);
}

TEST(io_context_impl_test, run_one_processes_single_task) {
  iocoro::detail::io_context_impl ctx;

  std::atomic<int> count{0};
  ctx.post([&] {
    ++count;
    ctx.post([&] { ++count; });
  });

  ctx.run_one();
  EXPECT_EQ(count.load(), 1);

  ctx.run_one();
  EXPECT_EQ(count.load(), 2);
}

TEST(io_context_impl_test, run_for_without_work_returns_zero) {
  iocoro::detail::io_context_impl ctx;

  auto n = ctx.run_for(std::chrono::milliseconds{1});
  EXPECT_EQ(n, 0U);
}

TEST(io_context_impl_test, schedule_timer_executes_callback) {
  iocoro::detail::io_context_impl ctx;

  std::atomic<bool> fired{false};
  std::atomic<bool> aborted{false};

  struct timer_state {
    std::atomic<bool>* fired;
    std::atomic<bool>* aborted;

    void on_complete() noexcept { fired->store(true); }
    void on_abort(std::error_code) noexcept { aborted->store(true); }
  };

  auto op = iocoro::detail::make_reactor_op<timer_state>(&fired, &aborted);
  (void)ctx.add_timer(std::chrono::steady_clock::now(), std::move(op));

  ctx.run_one();
  EXPECT_TRUE(fired.load());
  EXPECT_FALSE(aborted.load());
}

#include <gtest/gtest.h>

#include <xz/io/co_sleep.hpp>
#include <xz/io/co_spawn.hpp>
#include <xz/io/error.hpp>
#include <xz/io/io_context.hpp>
#include <xz/io/src.hpp>
#include <xz/io/steady_timer.hpp>
#include <xz/io/timer_handle.hpp>
#include <xz/io/use_awaitable.hpp>

#include <atomic>
#include <chrono>
#include <exception>

namespace {

TEST(io_context_test, post_and_run_executes_all_posted_operations) {
  xz::io::io_context ctx;
  auto ex = ctx.get_executor();
  std::atomic<int> n{0};

  ex.post([&] { n.fetch_add(1, std::memory_order_relaxed); });
  ex.post([&] { n.fetch_add(1, std::memory_order_relaxed); });

  EXPECT_EQ(ctx.run(), 2U);
  EXPECT_EQ(n.load(std::memory_order_relaxed), 2);
}

TEST(io_context_test, run_one_does_not_drain_work_posted_during_execution) {
  xz::io::io_context ctx;
  auto ex = ctx.get_executor();
  std::atomic<int> n{0};

  ex.post([&] {
    n.fetch_add(1, std::memory_order_relaxed);
    ex.post([&] { n.fetch_add(1, std::memory_order_relaxed); });
  });

  EXPECT_EQ(ctx.run_one(), 1U);
  EXPECT_EQ(n.load(std::memory_order_relaxed), 1);

  EXPECT_EQ(ctx.run_one(), 1U);
  EXPECT_EQ(n.load(std::memory_order_relaxed), 2);
}

TEST(io_context_test, schedule_timer_runs_callback_via_run_for) {
  using namespace std::chrono_literals;

  xz::io::io_context ctx;
  auto ex = ctx.get_executor();
  std::atomic<bool> fired{false};

  (void)ex.schedule_timer(10ms, [&] { fired.store(true, std::memory_order_relaxed); });

  auto const n = ctx.run_for(200ms);
  EXPECT_GE(n, 1U);
  EXPECT_TRUE(fired.load(std::memory_order_relaxed));
}

TEST(io_context_test, stop_prevents_run_and_restart_allows_processing) {
  xz::io::io_context ctx;
  auto ex = ctx.get_executor();
  std::atomic<int> n{0};

  ctx.stop();
  ex.post([&] { n.fetch_add(1, std::memory_order_relaxed); });

  EXPECT_EQ(ctx.run(), 0U);
  EXPECT_EQ(n.load(std::memory_order_relaxed), 0);

  ctx.restart();
  EXPECT_EQ(ctx.run(), 1U);
  EXPECT_EQ(n.load(std::memory_order_relaxed), 1);
}

TEST(io_context_test, co_sleep_resumes_via_timer_and_executor) {
  using namespace std::chrono_literals;

  xz::io::io_context ctx;
  std::atomic<bool> done{false};

  auto task = [&]() -> xz::io::awaitable<void> {
    co_await xz::io::co_sleep(10ms);
    done.store(true, std::memory_order_relaxed);
  };

  xz::io::co_spawn(ctx.get_executor(), task());

  (void)ctx.run_for(200ms);
  EXPECT_TRUE(done.load(std::memory_order_relaxed));
}

TEST(io_context_test, steady_timer_async_wait_resumes_on_fire) {
  using namespace std::chrono_literals;

  xz::io::io_context ctx;
  auto ex = ctx.get_executor();
  std::atomic<bool> done{false};
  std::atomic<bool> aborted{false};

  xz::io::steady_timer t{ex};
  t.expires_after(10ms);

  auto task = [&]() -> xz::io::awaitable<void> {
    auto ec = co_await t.async_wait(xz::io::use_awaitable);
    aborted.store(ec == xz::io::make_error_code(xz::io::error::operation_aborted),
                  std::memory_order_relaxed);
    done.store(true, std::memory_order_relaxed);
    co_return;
  };

  xz::io::co_spawn(ctx.get_executor(), task());

  (void)ctx.run_for(200ms);
  EXPECT_TRUE(done.load(std::memory_order_relaxed));
  EXPECT_FALSE(aborted.load(std::memory_order_relaxed));
}

TEST(io_context_test, steady_timer_async_wait_resumes_on_cancel) {
  using namespace std::chrono_literals;

  xz::io::io_context ctx;
  auto ex = ctx.get_executor();
  std::atomic<bool> done{false};
  std::atomic<bool> aborted{false};

  xz::io::steady_timer t{ex};
  t.expires_after(200ms);

  auto task = [&]() -> xz::io::awaitable<void> {
    auto ec = co_await t.async_wait(xz::io::use_awaitable);
    aborted.store(ec == xz::io::make_error_code(xz::io::error::operation_aborted),
                  std::memory_order_relaxed);
    done.store(true, std::memory_order_relaxed);
    co_return;
  };

  xz::io::co_spawn(ctx.get_executor(), task());

  // Let the coroutine start and suspend on async_wait, then cancel it.
  (void)ctx.run_one();
  (void)t.cancel();

  (void)ctx.run_for(50ms);
  EXPECT_TRUE(done.load(std::memory_order_relaxed));
  EXPECT_TRUE(aborted.load(std::memory_order_relaxed));
}

TEST(io_context_test, co_spawn_use_awaitable_returns_value) {
  xz::io::io_context ctx;
  auto ex = ctx.get_executor();

  std::atomic<bool> done{false};
  std::atomic<int> value{0};

  auto child = [ex]() -> xz::io::awaitable<int> {
    auto cur = co_await xz::io::this_coro::executor;
    EXPECT_EQ(cur, ex);
    co_return 42;
  };

  auto parent = [&]() -> xz::io::awaitable<void> {
    auto v = co_await xz::io::co_spawn(ex, child(), xz::io::use_awaitable);
    value.store(v, std::memory_order_relaxed);
    done.store(true, std::memory_order_relaxed);
  };

  xz::io::co_spawn(ex, parent());

  (void)ctx.run();
  EXPECT_TRUE(done.load(std::memory_order_relaxed));
  EXPECT_EQ(value.load(std::memory_order_relaxed), 42);
}

TEST(io_context_test, co_spawn_use_awaitable_rethrows_exception) {
  xz::io::io_context ctx;
  auto ex = ctx.get_executor();

  std::atomic<bool> got_exception{false};

  auto child = [ex]() -> xz::io::awaitable<int> {
    auto cur = co_await xz::io::this_coro::executor;
    EXPECT_EQ(cur, ex);
    throw std::runtime_error("boom");
  };

  auto parent = [&]() -> xz::io::awaitable<void> {
    try {
      (void)co_await xz::io::co_spawn(ex, child(), xz::io::use_awaitable);
    } catch (std::runtime_error const& e) {
      EXPECT_STREQ(e.what(), "boom");
      got_exception.store(true, std::memory_order_relaxed);
    }
  };

  xz::io::co_spawn(ex, parent());

  (void)ctx.run();
  EXPECT_TRUE(got_exception.load(std::memory_order_relaxed));
}

TEST(io_context_test, co_spawn_completion_callback_receives_value) {
  xz::io::io_context ctx;
  auto ex = ctx.get_executor();

  std::atomic<bool> called{false};
  std::atomic<int> value{0};

  auto child = []() -> xz::io::awaitable<int> { co_return 7; };

  xz::io::co_spawn(
    ex, child(),
    [&](xz::io::expected<int, std::exception_ptr> r) {
      EXPECT_TRUE(r.has_value());
      value.store(*r, std::memory_order_relaxed);
      called.store(true, std::memory_order_relaxed);
    });

  (void)ctx.run();
  EXPECT_TRUE(called.load(std::memory_order_relaxed));
  EXPECT_EQ(value.load(std::memory_order_relaxed), 7);
}

TEST(io_context_test, co_spawn_completion_callback_receives_exception) {
  xz::io::io_context ctx;
  auto ex = ctx.get_executor();

  std::atomic<bool> called{false};
  std::atomic<bool> got_runtime_error{false};

  auto child = []() -> xz::io::awaitable<int> {
    (void)co_await xz::io::this_coro::executor;
    throw std::runtime_error("fail");
  };

  xz::io::co_spawn(
    ex, child(),
    [&](xz::io::expected<int, std::exception_ptr> r) {
      EXPECT_FALSE(r.has_value());
      EXPECT_TRUE(static_cast<bool>(r.error()));
      try {
        std::rethrow_exception(r.error());
      } catch (std::runtime_error const& e) {
        EXPECT_STREQ(e.what(), "fail");
        got_runtime_error.store(true, std::memory_order_relaxed);
      }
      called.store(true, std::memory_order_relaxed);
    });

  (void)ctx.run();
  EXPECT_TRUE(called.load(std::memory_order_relaxed));
  EXPECT_TRUE(got_runtime_error.load(std::memory_order_relaxed));
}

}  // namespace

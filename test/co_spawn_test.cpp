#include <gtest/gtest.h>

#include <iocoro/co_sleep.hpp>
#include <iocoro/co_spawn.hpp>
#include <iocoro/expected.hpp>
#include <iocoro/io_context.hpp>
#include <iocoro/src.hpp>
#include <iocoro/this_coro.hpp>
#include <iocoro/use_awaitable.hpp>

#include <atomic>
#include <chrono>
#include <exception>

namespace {

TEST(co_spawn_test, co_spawn_use_awaitable_hot_starts_without_await) {
  iocoro::io_context ctx;
  auto ex = ctx.get_executor();

  std::atomic<bool> ran{false};

  auto child = [&]() -> iocoro::awaitable<void> {
    ran.store(true, std::memory_order_relaxed);
    co_return;
  };

  // Should start running immediately when co_spawn is called (once the context runs),
  // even if we never co_await the returned awaitable.
  auto unused = iocoro::co_spawn(ex, child(), iocoro::use_awaitable);
  (void)unused;

  (void)ctx.run();
  EXPECT_TRUE(ran.load(std::memory_order_relaxed));
}

TEST(co_spawn_test, co_spawn_use_awaitable_returns_value) {
  iocoro::io_context ctx;
  auto ex = ctx.get_executor();

  std::atomic<bool> done{false};
  std::atomic<int> value{0};

  auto child = [ex]() -> iocoro::awaitable<int> {
    auto cur = co_await iocoro::this_coro::executor;
    EXPECT_EQ(cur, ex);
    co_return 42;
  };

  auto parent = [&]() -> iocoro::awaitable<void> {
    auto v = co_await iocoro::co_spawn(ex, child(), iocoro::use_awaitable);
    value.store(v, std::memory_order_relaxed);
    done.store(true, std::memory_order_relaxed);
  };

  iocoro::co_spawn(ex, parent(), iocoro::detached);

  (void)ctx.run();
  EXPECT_TRUE(done.load(std::memory_order_relaxed));
  EXPECT_EQ(value.load(std::memory_order_relaxed), 42);
}

TEST(co_spawn_test, co_spawn_use_awaitable_rethrows_exception) {
  iocoro::io_context ctx;
  auto ex = ctx.get_executor();

  std::atomic<bool> got_exception{false};

  auto child = [ex]() -> iocoro::awaitable<int> {
    auto cur = co_await iocoro::this_coro::executor;
    EXPECT_EQ(cur, ex);
    throw std::runtime_error("boom");
  };

  auto parent = [&]() -> iocoro::awaitable<void> {
    try {
      (void)co_await iocoro::co_spawn(ex, child(), iocoro::use_awaitable);
    } catch (std::runtime_error const& e) {
      EXPECT_STREQ(e.what(), "boom");
      got_exception.store(true, std::memory_order_relaxed);
    }
  };

  iocoro::co_spawn(ex, parent(), iocoro::detached);

  (void)ctx.run();
  EXPECT_TRUE(got_exception.load(std::memory_order_relaxed));
}

TEST(co_spawn_test, co_spawn_use_awaitable_waits_for_timer_based_child) {
  using namespace std::chrono_literals;

  iocoro::io_context ctx;
  auto ex = ctx.get_executor();

  std::atomic<bool> done{false};

  auto slow = [&]() -> iocoro::awaitable<void> {
    co_await iocoro::co_sleep(10ms);
    done.store(true, std::memory_order_relaxed);
  };

  auto parent = [&]() -> iocoro::awaitable<void> {
    (void)co_await iocoro::co_spawn(ex, slow(), iocoro::use_awaitable);
    EXPECT_TRUE(done.load(std::memory_order_relaxed));
  };

  iocoro::co_spawn(ex, parent(), iocoro::detached);
  (void)ctx.run_for(200ms);

  EXPECT_TRUE(done.load(std::memory_order_relaxed));
}

TEST(co_spawn_test, co_spawn_completion_callback_receives_value) {
  iocoro::io_context ctx;
  auto ex = ctx.get_executor();

  std::atomic<bool> called{false};
  std::atomic<int> value{0};

  auto child = []() -> iocoro::awaitable<int> { co_return 7; };

  iocoro::co_spawn(ex, child(), [&](iocoro::expected<int, std::exception_ptr> r) {
    EXPECT_TRUE(r.has_value());
    value.store(*r, std::memory_order_relaxed);
    called.store(true, std::memory_order_relaxed);
  });

  (void)ctx.run();
  EXPECT_TRUE(called.load(std::memory_order_relaxed));
  EXPECT_EQ(value.load(std::memory_order_relaxed), 7);
}

TEST(co_spawn_test, co_spawn_completion_callback_receives_exception) {
  iocoro::io_context ctx;
  auto ex = ctx.get_executor();

  std::atomic<bool> called{false};
  std::atomic<bool> got_runtime_error{false};

  auto child = []() -> iocoro::awaitable<int> {
    (void)co_await iocoro::this_coro::executor;
    throw std::runtime_error("fail");
  };

  iocoro::co_spawn(ex, child(), [&](iocoro::expected<int, std::exception_ptr> r) {
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

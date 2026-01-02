#include <gtest/gtest.h>

#include <iocoro/co_sleep.hpp>
#include <iocoro/co_spawn.hpp>
#include <iocoro/completion_token.hpp>
#include <iocoro/expected.hpp>
#include <iocoro/io_context.hpp>
#include <iocoro/this_coro.hpp>

#include "test_util.hpp"

#include <atomic>
#include <chrono>
#include <exception>

namespace {

TEST(co_spawn_test, co_spawn_factory_detached_runs) {
  iocoro::io_context ctx;

  // Use the factory overload (callable returning awaitable) to avoid temporary-lambda pitfalls.
  auto ran = iocoro::sync_wait(ctx, [&]() -> iocoro::awaitable<bool> {
    (void)co_await iocoro::this_coro::executor;
    co_return true;
  }());

  EXPECT_TRUE(ran);
}

TEST(co_spawn_test, co_spawn_factory_completion_callback_receives_value) {
  iocoro::io_context ctx;
  auto ex = ctx.get_executor();

  std::atomic<bool> called{false};
  std::atomic<int> value{0};

  iocoro::co_spawn(
    ex, []() -> iocoro::awaitable<int> { co_return 7; },
    [&](iocoro::expected<int, std::exception_ptr> r) {
      EXPECT_TRUE(r.has_value());
      value.store(*r, std::memory_order_relaxed);
      called.store(true, std::memory_order_relaxed);
    });

  (void)ctx.run();
  EXPECT_TRUE(called.load(std::memory_order_relaxed));
  EXPECT_EQ(value.load(std::memory_order_relaxed), 7);
}

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

TEST(co_spawn_test, co_spawn_use_awaitable_rethrows_exception) {
  iocoro::io_context ctx;
  auto ex = ctx.get_executor();

  auto child = [ex]() -> iocoro::awaitable<int> {
    throw std::runtime_error("boom");
  };

  auto got_exception = iocoro::sync_wait(ctx, [&]() -> iocoro::awaitable<bool> {
    try {
      (void)co_await iocoro::co_spawn(ex, child(), iocoro::use_awaitable);
    } catch (std::runtime_error const& e) {
      EXPECT_STREQ(e.what(), "boom");
      co_return true;
    }
    co_return false;
  }());

  EXPECT_TRUE(got_exception);
}

TEST(co_spawn_test, co_spawn_use_awaitable_waits_for_timer_based_child) {
  using namespace std::chrono_literals;

  iocoro::io_context ctx;
  auto ex = ctx.get_executor();

  auto slow = [&]() -> iocoro::awaitable<int> {
    co_await iocoro::co_sleep(10ms);
    co_return 7;
  };

  auto v = iocoro::sync_wait_for(ctx, 200ms, [&]() -> iocoro::awaitable<int> {
    return iocoro::co_spawn(ex, slow(), iocoro::use_awaitable);
  }());

  EXPECT_EQ(v, 7);
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

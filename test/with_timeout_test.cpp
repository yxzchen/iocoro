#include <gtest/gtest.h>

#include <iocoro/co_sleep.hpp>
#include <iocoro/error.hpp>
#include <iocoro/expected.hpp>
#include <iocoro/with_timeout.hpp>
#include <iocoro/io_context.hpp>
#include <iocoro/this_coro.hpp>

#include "test_util.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <span>
#include <system_error>

namespace {

using namespace std::chrono_literals;

struct cancellable_test_stream {
  std::atomic<int> cancel_calls{0};
  std::atomic<int> cancel_read_calls{0};
  std::atomic<int> cancel_write_calls{0};

  std::atomic<bool> cancelled{false};
  std::atomic<bool> cancelled_read{false};
  std::atomic<bool> cancelled_write{false};

  iocoro::io_executor ex{};

  explicit cancellable_test_stream(iocoro::io_executor ex_) : ex(ex_) {}

  auto get_executor() const noexcept -> iocoro::io_executor { return ex; }

  void cancel() noexcept {
    cancel_calls.fetch_add(1, std::memory_order_relaxed);
    cancelled.store(true, std::memory_order_release);
  }
  void cancel_read() noexcept {
    cancel_read_calls.fetch_add(1, std::memory_order_relaxed);
    cancelled_read.store(true, std::memory_order_release);
  }
  void cancel_write() noexcept {
    cancel_write_calls.fetch_add(1, std::memory_order_relaxed);
    cancelled_write.store(true, std::memory_order_release);
  }

  auto async_read_some(std::span<std::byte> buf)
    -> iocoro::awaitable<iocoro::expected<std::size_t, std::error_code>> {
    (void)buf;
    auto tok = co_await iocoro::this_coro::cancellation_token;
    for (int i = 0; i < 200; ++i) {
      if (tok.stop_requested() ||
          cancelled.load(std::memory_order_acquire) ||
          cancelled_read.load(std::memory_order_acquire)) {
        co_return iocoro::unexpected(iocoro::error::operation_aborted);
      }
      co_await iocoro::co_sleep(1ms);
    }
    co_return iocoro::unexpected(iocoro::error::timed_out);
  }

  auto async_write_some(std::span<std::byte const> buf)
    -> iocoro::awaitable<iocoro::expected<std::size_t, std::error_code>> {
    (void)buf;
    auto tok = co_await iocoro::this_coro::cancellation_token;
    for (int i = 0; i < 200; ++i) {
      if (tok.stop_requested() ||
          cancelled.load(std::memory_order_acquire) ||
          cancelled_write.load(std::memory_order_acquire)) {
        co_return iocoro::unexpected(iocoro::error::operation_aborted);
      }
      co_await iocoro::co_sleep(1ms);
    }
    co_return iocoro::unexpected(iocoro::error::timed_out);
  }
};

TEST(with_timeout_test, this_coro_with_timeout_allows_completion_before_deadline) {
  iocoro::io_context ctx;

  auto r =
    iocoro::sync_wait(ctx, [&]() -> iocoro::awaitable<iocoro::expected<int, std::error_code>> {
      auto scope = co_await iocoro::this_coro::scoped_timeout(200ms);
      co_await iocoro::co_sleep(5ms);
      co_return 42;
    }());

  ASSERT_TRUE(r) << r.error().message();
  EXPECT_EQ(*r, 42);
}

TEST(with_timeout_test, this_coro_with_timeout_allows_success_error_code_before_deadline) {
  iocoro::io_context ctx;

  auto r = iocoro::sync_wait(ctx, [&]() -> iocoro::awaitable<std::error_code> {
    auto scope = co_await iocoro::this_coro::scoped_timeout(200ms);
    co_await iocoro::co_sleep(5ms);
    co_return std::error_code{};
  }());

  EXPECT_FALSE(static_cast<bool>(r)) << r.message();
}

TEST(with_timeout_test, scoped_timeout_fired_becomes_true_on_deadline) {
  iocoro::io_context ctx;

  auto r = iocoro::sync_wait_for(ctx, 500ms, []() -> iocoro::awaitable<std::error_code> {
    auto scope = co_await iocoro::this_coro::scoped_timeout(10ms);
    for (int i = 0; i < 200; ++i) {
      auto tok = co_await iocoro::this_coro::cancellation_token;
      if (tok.stop_requested()) {
        EXPECT_TRUE(scope.timed_out());
        co_return iocoro::error::operation_aborted;
      }
      co_await iocoro::co_sleep(1ms);
    }
    co_return iocoro::error::timed_out;
  }());

  EXPECT_TRUE(static_cast<bool>(r)) << r.message();
  EXPECT_EQ(r, iocoro::error::operation_aborted);
}

TEST(with_timeout_test, detached_timeout_returns_timed_out_without_waiting_expected) {
  using namespace std::chrono_literals;

  iocoro::io_context ctx;

  // Use a very short operation time to ensure it completes quickly after timeout
  auto r = iocoro::sync_wait_for(
    ctx, 500ms, []() -> iocoro::awaitable<iocoro::expected<int, std::error_code>> {
      co_return co_await iocoro::with_timeout_detached(
        []() -> iocoro::awaitable<iocoro::expected<int, std::error_code>> {
          co_await iocoro::co_sleep(10ms);
          co_return 7;
        }(),
        5ms);
    }());

  ASSERT_FALSE(r);
  EXPECT_EQ(r.error(), iocoro::error::timed_out);
}

}  // namespace

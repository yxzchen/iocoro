#include <gtest/gtest.h>

#include <iocoro/co_sleep.hpp>
#include <iocoro/error.hpp>
#include <iocoro/expected.hpp>
#include <iocoro/with_timeout.hpp>
#include <iocoro/io_context.hpp>

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

  auto async_read_some(std::span<std::byte> buf, iocoro::cancellation_token tok = {})
    -> iocoro::awaitable<iocoro::expected<std::size_t, std::error_code>> {
    (void)buf;
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

  auto async_write_some(std::span<std::byte const> buf, iocoro::cancellation_token tok = {})
    -> iocoro::awaitable<iocoro::expected<std::size_t, std::error_code>> {
    (void)buf;
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

TEST(with_timeout_test, completes_before_timeout_returns_value) {
  iocoro::io_context ctx;

  auto r =
    iocoro::sync_wait(ctx, [&]() -> iocoro::awaitable<iocoro::expected<int, std::error_code>> {
      co_return co_await iocoro::with_timeout(
        [&](iocoro::cancellation_token) -> iocoro::awaitable<iocoro::expected<int, std::error_code>> {
          co_await iocoro::co_sleep(5ms);
          co_return 42;
        },
        200ms);
    }());

  ASSERT_TRUE(r) << r.error().message();
  EXPECT_EQ(*r, 42);
}

TEST(with_timeout_test,
     error_code_completes_before_timeout_returns_success) {
  iocoro::io_context ctx;

  auto r = iocoro::sync_wait(ctx, [&]() -> iocoro::awaitable<std::error_code> {
    co_return co_await iocoro::with_timeout(
      [&](iocoro::cancellation_token) -> iocoro::awaitable<std::error_code> {
        co_await iocoro::co_sleep(5ms);
        co_return std::error_code{};
      },
      200ms);
  }());

  EXPECT_FALSE(static_cast<bool>(r)) << r.message();
}

TEST(with_timeout_test, timeout_maps_operation_aborted_to_timed_out) {
  iocoro::io_context ctx;

  auto r = iocoro::sync_wait_for(
    ctx, 500ms, [&]() -> iocoro::awaitable<iocoro::expected<int, std::error_code>> {
      co_return co_await iocoro::with_timeout(
        [&](iocoro::cancellation_token tok) -> iocoro::awaitable<iocoro::expected<int, std::error_code>> {
          // Wait until cancelled, then surface operation_aborted.
          for (int i = 0; i < 200; ++i) {
            if (tok.stop_requested()) {
              co_return iocoro::unexpected(iocoro::error::operation_aborted);
            }
            co_await iocoro::co_sleep(1ms);
          }
          co_return iocoro::unexpected(iocoro::error::timed_out);
        },
        10ms);
    }());

  ASSERT_FALSE(r);
  EXPECT_EQ(r.error(), iocoro::error::timed_out);
}

TEST(with_timeout_test,
     error_code_timeout_maps_operation_aborted_to_timed_out) {
  iocoro::io_context ctx;

  auto r = iocoro::sync_wait_for(ctx, 500ms, [&]() -> iocoro::awaitable<std::error_code> {
    co_return co_await iocoro::with_timeout(
      [&](iocoro::cancellation_token tok) -> iocoro::awaitable<std::error_code> {
        // Wait until cancelled, then surface operation_aborted.
        for (int i = 0; i < 200; ++i) {
          if (tok.stop_requested()) {
            co_return iocoro::error::operation_aborted;
          }
          co_await iocoro::co_sleep(1ms);
        }
        co_return iocoro::error::timed_out;
      },
      10ms);
  }());

  EXPECT_TRUE(static_cast<bool>(r)) << r.message();
  EXPECT_EQ(r, iocoro::error::timed_out);
}

TEST(with_timeout_test, external_operation_aborted_is_not_mapped_to_timed_out) {
  iocoro::io_context ctx;

  auto r = iocoro::sync_wait_for(
    ctx, 500ms, [&]() -> iocoro::awaitable<iocoro::expected<int, std::error_code>> {
      co_return co_await iocoro::with_timeout(
        [&](iocoro::cancellation_token) -> iocoro::awaitable<iocoro::expected<int, std::error_code>> {
          // External cancellation: complete with operation_aborted before timeout can fire.
          co_return iocoro::unexpected(iocoro::error::operation_aborted);
        },
        200ms);
    }());

  ASSERT_FALSE(r);
  EXPECT_EQ(r.error(), iocoro::error::operation_aborted);
}

TEST(with_timeout_test, timeout_does_not_map_non_operation_aborted_error) {
  iocoro::io_context ctx;

  auto r = iocoro::sync_wait_for(
    ctx, 500ms, [&]() -> iocoro::awaitable<iocoro::expected<int, std::error_code>> {
      co_return co_await iocoro::with_timeout(
        [&](iocoro::cancellation_token) -> iocoro::awaitable<iocoro::expected<int, std::error_code>> {
          co_await iocoro::co_sleep(20ms);  // ensure the timer has time to fire
          co_return iocoro::unexpected(iocoro::error::broken_pipe);
        },
        5ms);
    }());

  ASSERT_FALSE(r);
  EXPECT_EQ(r.error(), iocoro::error::broken_pipe);
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

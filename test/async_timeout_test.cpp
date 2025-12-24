#include <gtest/gtest.h>

#include <iocoro/co_sleep.hpp>
#include <iocoro/error.hpp>
#include <iocoro/expected.hpp>
#include <iocoro/impl.hpp>
#include <iocoro/io/async_read.hpp>
#include <iocoro/io/async_write.hpp>
#include <iocoro/io_context.hpp>

#include "test_util.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <span>
#include <system_error>

namespace {

using namespace std::chrono_literals;

struct slow_cancellable_stream {
  std::atomic<bool> cancelled_read{false};
  std::atomic<bool> cancelled_write{false};
  std::atomic<bool> in_read{false};
  std::atomic<bool> in_write{false};

  void cancel() noexcept {
    cancelled_read.store(true, std::memory_order_release);
    cancelled_write.store(true, std::memory_order_release);
  }
  void cancel_read() noexcept { cancelled_read.store(true, std::memory_order_release); }
  void cancel_write() noexcept { cancelled_write.store(true, std::memory_order_release); }

  auto async_read_some(std::span<std::byte> buf)
    -> iocoro::awaitable<iocoro::expected<std::size_t, std::error_code>> {
    (void)buf;
    in_read.store(true, std::memory_order_release);
    auto guard = [&]() { in_read.store(false, std::memory_order_release); };

    // Wait until cancelled (yields to executor).
    for (int i = 0; i < 200; ++i) {
      if (cancelled_read.load(std::memory_order_acquire)) {
        guard();
        co_return iocoro::unexpected(iocoro::error::operation_aborted);
      }
      co_await iocoro::co_sleep(1ms);
    }

    guard();
    co_return iocoro::expected<std::size_t, std::error_code>(0);
  }

  auto async_write_some(std::span<std::byte const> buf)
    -> iocoro::awaitable<iocoro::expected<std::size_t, std::error_code>> {
    (void)buf;
    in_write.store(true, std::memory_order_release);
    auto guard = [&]() { in_write.store(false, std::memory_order_release); };

    // Wait until cancelled (yields to executor).
    for (int i = 0; i < 200; ++i) {
      if (cancelled_write.load(std::memory_order_acquire)) {
        guard();
        co_return iocoro::unexpected(iocoro::error::operation_aborted);
      }
      co_await iocoro::co_sleep(1ms);
    }

    guard();
    co_return iocoro::expected<std::size_t, std::error_code>(0);
  }
};

TEST(async_timeout_test, async_read_some_timeout_returns_timed_out_and_cleans_up) {
  iocoro::io_context ctx;
  slow_cancellable_stream s{};

  std::array<std::byte, 1> buf{};

  auto r = iocoro::sync_wait_for(
    ctx, 200ms, [&]() -> iocoro::awaitable<iocoro::expected<std::size_t, std::error_code>> {
      return iocoro::io::async_read_some_timeout(s, buf, 10ms);
    }());

  ASSERT_FALSE(r);
  EXPECT_EQ(r.error(), iocoro::error::timed_out);
  EXPECT_FALSE(s.in_read.load(std::memory_order_acquire));
  EXPECT_FALSE(s.cancelled_write.load(std::memory_order_acquire));
}

TEST(async_timeout_test, async_read_timeout_returns_timed_out_and_cleans_up) {
  iocoro::io_context ctx;
  slow_cancellable_stream s{};

  std::array<std::byte, 8> buf{};

  auto r = iocoro::sync_wait_for(
    ctx, 200ms, [&]() -> iocoro::awaitable<iocoro::expected<std::size_t, std::error_code>> {
      return iocoro::io::async_read_timeout(s, buf, 10ms);
    }());

  ASSERT_FALSE(r);
  EXPECT_EQ(r.error(), iocoro::error::timed_out);
  EXPECT_FALSE(s.in_read.load(std::memory_order_acquire));
  EXPECT_FALSE(s.cancelled_write.load(std::memory_order_acquire));
}

TEST(async_timeout_test, async_write_some_timeout_returns_timed_out_and_cleans_up) {
  iocoro::io_context ctx;
  slow_cancellable_stream s{};

  std::array<std::byte, 1> buf{};

  auto r = iocoro::sync_wait_for(
    ctx, 200ms, [&]() -> iocoro::awaitable<iocoro::expected<std::size_t, std::error_code>> {
      return iocoro::io::async_write_some_timeout(s, buf, 10ms);
    }());

  ASSERT_FALSE(r);
  EXPECT_EQ(r.error(), iocoro::error::timed_out);
  EXPECT_FALSE(s.in_write.load(std::memory_order_acquire));
  EXPECT_FALSE(s.cancelled_read.load(std::memory_order_acquire));
}

TEST(async_timeout_test, async_write_timeout_returns_timed_out_and_cleans_up) {
  iocoro::io_context ctx;
  slow_cancellable_stream s{};

  std::array<std::byte, 8> buf{};

  auto r = iocoro::sync_wait_for(
    ctx, 200ms, [&]() -> iocoro::awaitable<iocoro::expected<std::size_t, std::error_code>> {
      return iocoro::io::async_write_timeout(s, buf, 10ms);
    }());

  ASSERT_FALSE(r);
  EXPECT_EQ(r.error(), iocoro::error::timed_out);
  EXPECT_FALSE(s.in_write.load(std::memory_order_acquire));
  EXPECT_FALSE(s.cancelled_read.load(std::memory_order_acquire));
}

TEST(async_timeout_test, external_cancel_is_propagated_not_mapped_to_timed_out) {
  iocoro::io_context ctx;
  slow_cancellable_stream s{};
  s.cancel_read();  // external cancellation (read-side)

  std::array<std::byte, 1> buf{};

  auto r = iocoro::sync_wait_for(
    ctx, 200ms, [&]() -> iocoro::awaitable<iocoro::expected<std::size_t, std::error_code>> {
      // Large timeout so it is not the reason for cancellation.
      return iocoro::io::async_read_some_timeout(s, buf, 1s);
    }());

  ASSERT_FALSE(r);
  EXPECT_EQ(r.error(), iocoro::error::operation_aborted);
}

}  // namespace

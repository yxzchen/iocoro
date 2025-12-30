#include <gtest/gtest.h>

#include <iocoro/co_sleep.hpp>
#include <iocoro/error.hpp>
#include <iocoro/expected.hpp>
#include <iocoro/impl.hpp>
#include <iocoro/io/with_timeout.hpp>
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
    for (int i = 0; i < 200; ++i) {
      if (cancelled.load(std::memory_order_acquire) ||
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
    for (int i = 0; i < 200; ++i) {
      if (cancelled.load(std::memory_order_acquire) ||
          cancelled_write.load(std::memory_order_acquire)) {
        co_return iocoro::unexpected(iocoro::error::operation_aborted);
      }
      co_await iocoro::co_sleep(1ms);
    }
    co_return iocoro::unexpected(iocoro::error::timed_out);
  }
};

TEST(with_timeout_test, completes_before_timeout_returns_value_and_does_not_call_on_timeout) {
  iocoro::io_context ctx;

  std::atomic<bool> called{false};
  auto r =
    iocoro::sync_wait(ctx, [&]() -> iocoro::awaitable<iocoro::expected<int, std::error_code>> {
      co_return co_await iocoro::io::with_timeout(
        [&]() -> iocoro::awaitable<iocoro::expected<int, std::error_code>> {
          co_await iocoro::co_sleep(5ms);
          co_return 42;
        }(),
        200ms, [&]() { called = true; });
    }());

  ASSERT_TRUE(r) << r.error().message();
  EXPECT_EQ(*r, 42);
  EXPECT_FALSE(called.load(std::memory_order_relaxed));
}

TEST(with_timeout_test, error_code_completes_before_timeout_returns_success_and_does_not_call_on_timeout) {
  iocoro::io_context ctx;

  std::atomic<bool> called{false};
  auto r = iocoro::sync_wait(ctx, [&]() -> iocoro::awaitable<std::error_code> {
    co_return co_await iocoro::io::with_timeout(
      [&]() -> iocoro::awaitable<std::error_code> {
        co_await iocoro::co_sleep(5ms);
        co_return std::error_code{};
      }(),
      200ms, [&]() { called = true; });
  }());

  EXPECT_FALSE(static_cast<bool>(r)) << r.message();
  EXPECT_FALSE(called.load(std::memory_order_relaxed));
}

TEST(with_timeout_test, timeout_maps_operation_aborted_to_timed_out_and_calls_on_timeout) {
  iocoro::io_context ctx;

  std::atomic<bool> cancelled{false};
  std::atomic<bool> called{false};

  auto r = iocoro::sync_wait_for(
    ctx, 500ms, [&]() -> iocoro::awaitable<iocoro::expected<int, std::error_code>> {
      co_return co_await iocoro::io::with_timeout(
        [&]() -> iocoro::awaitable<iocoro::expected<int, std::error_code>> {
          // Wait until cancelled, then surface operation_aborted.
          for (int i = 0; i < 200; ++i) {
            if (cancelled.load(std::memory_order_acquire)) {
              co_return iocoro::unexpected(iocoro::error::operation_aborted);
            }
            co_await iocoro::co_sleep(1ms);
          }
          co_return iocoro::unexpected(iocoro::error::timed_out);
        }(),
        10ms,
        [&]() {
          called.store(true, std::memory_order_release);
          cancelled.store(true, std::memory_order_release);
        });
    }());

  ASSERT_FALSE(r);
  EXPECT_TRUE(called.load(std::memory_order_acquire));
  EXPECT_EQ(r.error(), iocoro::error::timed_out);
}

TEST(with_timeout_test, error_code_timeout_maps_operation_aborted_to_timed_out_and_calls_on_timeout) {
  iocoro::io_context ctx;

  std::atomic<bool> cancelled{false};
  std::atomic<bool> called{false};

  auto r = iocoro::sync_wait_for(ctx, 500ms, [&]() -> iocoro::awaitable<std::error_code> {
    co_return co_await iocoro::io::with_timeout(
      [&]() -> iocoro::awaitable<std::error_code> {
        // Wait until cancelled, then surface operation_aborted.
        for (int i = 0; i < 200; ++i) {
          if (cancelled.load(std::memory_order_acquire)) {
            co_return iocoro::error::operation_aborted;
          }
          co_await iocoro::co_sleep(1ms);
        }
        co_return iocoro::error::timed_out;
      }(),
      10ms,
      [&]() {
        called.store(true, std::memory_order_release);
        cancelled.store(true, std::memory_order_release);
      });
  }());

  EXPECT_TRUE(static_cast<bool>(r)) << r.message();
  EXPECT_TRUE(called.load(std::memory_order_acquire));
  EXPECT_EQ(r, iocoro::error::timed_out);
}

TEST(with_timeout_test, external_operation_aborted_is_not_mapped_to_timed_out) {
  iocoro::io_context ctx;

  std::atomic<bool> called{false};

  auto r = iocoro::sync_wait_for(
    ctx, 500ms, [&]() -> iocoro::awaitable<iocoro::expected<int, std::error_code>> {
      co_return co_await iocoro::io::with_timeout(
        [&]() -> iocoro::awaitable<iocoro::expected<int, std::error_code>> {
          // External cancellation: complete with operation_aborted before timeout can fire.
          co_return iocoro::unexpected(iocoro::error::operation_aborted);
        }(),
        200ms, [&]() { called = true; });
    }());

  ASSERT_FALSE(r);
  EXPECT_EQ(r.error(), iocoro::error::operation_aborted);
  EXPECT_FALSE(called.load(std::memory_order_relaxed));
}

TEST(with_timeout_test, timeout_does_not_map_non_operation_aborted_error) {
  iocoro::io_context ctx;

  std::atomic<bool> called{false};
  auto r = iocoro::sync_wait_for(
    ctx, 500ms, [&]() -> iocoro::awaitable<iocoro::expected<int, std::error_code>> {
      co_return co_await iocoro::io::with_timeout(
        [&]() -> iocoro::awaitable<iocoro::expected<int, std::error_code>> {
          co_await iocoro::co_sleep(20ms);  // ensure the timer has time to fire
          co_return iocoro::unexpected(iocoro::error::broken_pipe);
        }(),
        5ms, [&]() { called = true; });
    }());

  ASSERT_FALSE(r);
  EXPECT_TRUE(called.load(std::memory_order_acquire));
  EXPECT_EQ(r.error(), iocoro::error::broken_pipe);
}

TEST(with_timeout_test, with_timeout_stream_overload_uses_cancel) {
  iocoro::io_context ctx;
  cancellable_test_stream s{};

  std::array<std::byte, 1> buf{};

  auto r = iocoro::sync_wait_for(
    ctx, 500ms, [&]() -> iocoro::awaitable<iocoro::expected<std::size_t, std::error_code>> {
      return iocoro::io::with_timeout(s, s.async_read_some(buf), 5ms);
    }());

  ASSERT_FALSE(r);
  EXPECT_EQ(r.error(), iocoro::error::timed_out);
  EXPECT_GE(s.cancel_calls.load(std::memory_order_relaxed), 1);
}

TEST(with_timeout_test, with_timeout_read_prefers_cancel_read) {
  iocoro::io_context ctx;
  cancellable_test_stream s{};

  std::array<std::byte, 1> buf{};

  auto r = iocoro::sync_wait_for(
    ctx, 500ms, [&]() -> iocoro::awaitable<iocoro::expected<std::size_t, std::error_code>> {
      return iocoro::io::with_timeout_read(s, s.async_read_some(buf), 5ms);
    }());

  ASSERT_FALSE(r);
  EXPECT_EQ(r.error(), iocoro::error::timed_out);
  EXPECT_GE(s.cancel_read_calls.load(std::memory_order_relaxed), 1);
  EXPECT_EQ(s.cancel_write_calls.load(std::memory_order_relaxed), 0);
}

TEST(with_timeout_test, with_timeout_write_prefers_cancel_write) {
  iocoro::io_context ctx;
  cancellable_test_stream s{};

  std::array<std::byte, 1> buf{};

  auto r = iocoro::sync_wait_for(
    ctx, 500ms, [&]() -> iocoro::awaitable<iocoro::expected<std::size_t, std::error_code>> {
      return iocoro::io::with_timeout_write(s, s.async_write_some(buf), 5ms);
    }());

  ASSERT_FALSE(r);
  EXPECT_EQ(r.error(), iocoro::error::timed_out);
  EXPECT_GE(s.cancel_write_calls.load(std::memory_order_relaxed), 1);
  EXPECT_EQ(s.cancel_read_calls.load(std::memory_order_relaxed), 0);
}

TEST(with_timeout_test, detached_timeout_returns_timed_out_without_waiting_expected) {
  using namespace std::chrono_literals;

  iocoro::io_context ctx;
  auto ex = ctx.get_executor();
  ctx.restart();

  std::atomic<bool> op_completed{false};

  std::atomic<bool> done{false};
  std::optional<iocoro::expected<int, std::error_code>> out{};
  std::exception_ptr ep{};

  auto main = [&]() -> iocoro::awaitable<iocoro::expected<int, std::error_code>> {
    co_return co_await iocoro::io::with_timeout_detached(
      [&]() -> iocoro::awaitable<iocoro::expected<int, std::error_code>> {
        co_await iocoro::co_sleep(200ms);
        op_completed.store(true, std::memory_order_release);
        co_return 7;
      }(),
      5ms);
  };

  iocoro::co_spawn(
    ex, main(),
    [&](iocoro::expected<iocoro::expected<int, std::error_code>, std::exception_ptr> r) mutable {
      done.store(true, std::memory_order_release);
      if (!r) {
        ep = std::move(r).error();
      } else {
        out.emplace(std::move(*r));
      }
      ctx.stop();  // don't drain detached background work
    });

  (void)ctx.run_for(50ms);

  ASSERT_TRUE(done.load(std::memory_order_acquire));
  if (ep) {
    std::rethrow_exception(ep);
  }
  ASSERT_TRUE(out.has_value());
  ASSERT_FALSE(*out);
  EXPECT_EQ(out->error(), iocoro::error::timed_out);
  EXPECT_FALSE(op_completed.load(std::memory_order_acquire));
}

TEST(with_timeout_test, detached_timeout_returns_timed_out_without_waiting_error_code) {
  using namespace std::chrono_literals;

  iocoro::io_context ctx;
  auto ex = ctx.get_executor();
  ctx.restart();

  std::atomic<bool> op_completed{false};

  std::atomic<bool> done{false};
  std::optional<std::error_code> out{};
  std::exception_ptr ep{};

  auto main = [&]() -> iocoro::awaitable<std::error_code> {
    co_return co_await iocoro::io::with_timeout_detached(
      [&]() -> iocoro::awaitable<std::error_code> {
        co_await iocoro::co_sleep(200ms);
        op_completed.store(true, std::memory_order_release);
        co_return std::error_code{};
      }(),
      5ms);
  };

  iocoro::co_spawn(ex, main(), [&](iocoro::expected<std::error_code, std::exception_ptr> r) mutable {
    done.store(true, std::memory_order_release);
    if (!r) {
      ep = std::move(r).error();
    } else {
      out.emplace(std::move(*r));
    }
    ctx.stop();  // don't drain detached background work
  });

  (void)ctx.run_for(50ms);

  ASSERT_TRUE(done.load(std::memory_order_acquire));
  if (ep) {
    std::rethrow_exception(ep);
  }
  ASSERT_TRUE(out.has_value());
  EXPECT_EQ(*out, iocoro::error::timed_out);
  EXPECT_FALSE(op_completed.load(std::memory_order_acquire));
}

}  // namespace

#pragma once

#include <iocoro/assert.hpp>
#include <iocoro/awaitable.hpp>
#include <iocoro/co_spawn.hpp>
#include <iocoro/completion_token.hpp>
#include <iocoro/error.hpp>
#include <iocoro/expected.hpp>
#include <iocoro/io/stream_concepts.hpp>
#include <iocoro/io_executor.hpp>
#include <iocoro/steady_timer.hpp>
#include <iocoro/when_any.hpp>

#include <atomic>
#include <chrono>
#include <concepts>
#include <memory>
#include <system_error>
#include <type_traits>
#include <utility>

namespace iocoro::io {

namespace detail {

template <class Result>
struct timeout_result_traits;

template <class T>
struct timeout_result_traits<iocoro::expected<T, std::error_code>> {
  using result_type = iocoro::expected<T, std::error_code>;

  static auto is_operation_aborted(result_type const& r) -> bool {
    if (!r && r.error() == error::operation_aborted) {
      return true;
    }
    return false;
  }

  static auto from_error(std::error_code ec) -> result_type { return unexpected(ec); }

  static auto timed_out() -> result_type { return unexpected(error::timed_out); }
};

template <>
struct timeout_result_traits<std::error_code> {
  using result_type = std::error_code;

  static auto is_operation_aborted(result_type const& r) -> bool {
    if (r == error::operation_aborted) {
      return true;
    }
    return false;
  }

  static auto from_error(std::error_code ec) -> result_type { return ec; }

  static auto timed_out() -> result_type { return error::timed_out; }
};

template <class Result, class Rep, class Period, class OnTimeout>
  requires std::invocable<OnTimeout&>
auto with_timeout_impl(io_executor ex, awaitable<Result> op, std::chrono::duration<Rep, Period> timeout,
                       OnTimeout&& on_timeout) -> awaitable<Result> {
  using traits = timeout_result_traits<Result>;

  if (timeout <= std::chrono::duration<Rep, Period>::zero()) {
    // Cannot return immediately, because it would skip the timeout callback, potentially causing
    // resource leaks or inconsistent state. Even if the timeout is zero, we must invoke on_timeout
    // and co_await op before returning timed_out.
    on_timeout();
    auto r = co_await std::move(op);

    if (traits::is_operation_aborted(r)) {
      co_return traits::timed_out();
    }
    co_return r;
  }

  IOCORO_ENSURE(ex, "with_timeout: requires a non-empty io_executor");

  auto timer = std::make_shared<steady_timer>(ex);
  (void)timer->expires_after(std::chrono::duration_cast<steady_timer::duration>(timeout));

  std::atomic<bool> fired{false};

  // Spawn the timeout watcher and join it before returning so we don't leak cancellation work.
  auto watcher = co_spawn(
    ex,
    [timer, &fired, on_timeout = std::forward<OnTimeout>(on_timeout)]() mutable -> awaitable<void> {
      auto ec = co_await timer->async_wait(use_awaitable);
      if (!ec) {
        fired.store(true, std::memory_order_release);
        on_timeout();
      }
      co_return;
    },
    use_awaitable);

  auto r = co_await std::move(op);

  // Stop the watcher (best-effort) and wait for it to exit.
  (void)timer->cancel();
  (void)co_await std::move(watcher);

  if (fired.load(std::memory_order_acquire) && traits::is_operation_aborted(r)) {
    co_return traits::timed_out();
  }
  co_return r;
}

template <class Result, class Rep, class Period>
auto with_timeout_detached_impl(io_executor ex, awaitable<Result> op,
                                std::chrono::duration<Rep, Period> timeout)
  -> awaitable<Result> {
  using traits = timeout_result_traits<Result>;

  IOCORO_ENSURE(ex, "with_timeout_detached: requires a non-empty io_executor");

  auto timer = std::make_shared<steady_timer>(ex);
  (void)timer->expires_after(std::chrono::duration_cast<steady_timer::duration>(timeout));

  auto timer_wait = [timer]() -> awaitable<std::error_code> {
    co_return co_await timer->async_wait(use_awaitable);
  };

  // Start both concurrently; whichever finishes first determines the result.
  // NOTE: when_any does not cancel the losing task.
  auto [index, v] = co_await when_any(std::move(op), timer_wait());

  if (index == 0) {
    (void)timer->cancel();
    co_return std::get<0>(std::move(v));
  }

  auto ec = std::get<1>(std::move(v));
  if (!ec) {
    co_return traits::timed_out();
  }

  // Timer wait completed due to cancellation/executor shutdown.
  // Treat it as a timer error rather than a timeout.
  co_return traits::from_error(ec);
}

}  // namespace detail

/// Await an I/O awaitable with a deadline.
///
/// Contract:
/// - `op` must be safe to cancel via `on_timeout()` (e.g. it is waiting on a stream operation
///   that returns `error::operation_aborted` when cancelled).
/// - This function will NOT return early on timeout unless it can request cancellation and
///   then observe the underlying operation exit. This prevents "background I/O continuing
///   after timeout", which is critical when user buffers are involved.
/// - If `op` is a "lazy" awaitable that does not start the underlying I/O immediately,
///   there is a window where the timeout watcher may fire before the operation has
///   registered any file descriptors or handles. In this case, the cancellation triggered
///   by the timeout may have no effect. For strict timeout enforcement, ensure that `op`
///   begins its I/O promptly (e.g., via eager co_await) when using very short timeouts.
///
/// Semantics:
/// - On timeout, calls `on_timeout()` (best-effort) and returns `error::timed_out` iff the
///   operation completes with `error::operation_aborted` and the timeout actually fired.
/// - If the operation is cancelled externally (not by this timer), the original
///   `error::operation_aborted` is propagated.
template <class Result, class Rep, class Period, class OnTimeout>
  requires std::invocable<OnTimeout&>
auto with_timeout(io_executor ex, awaitable<Result> op, std::chrono::duration<Rep, Period> timeout,
                  OnTimeout&& on_timeout) -> awaitable<Result> {
  co_return co_await detail::with_timeout_impl(ex, std::move(op), timeout,
                                               std::forward<OnTimeout>(on_timeout));
}

/// Await an I/O awaitable with a deadline (detached semantics).
///
/// Contract:
/// - Unlike `with_timeout`, this function may return on timeout without waiting for `op` to finish.
///   The underlying operation may continue in the background after this returns.
/// - This is only safe when `op` does not hold references to memory that may be freed after timeout
///   (e.g. user buffers). Prefer `with_timeout` for buffer-based I/O.
///
/// Semantics:
/// - Races `op` against a timer.
/// - If the timer fires first, returns `error::timed_out` and does not attempt to cancel `op`.
/// - If `op` finishes first, returns its result and does not call `on_timeout()`.
template <class Result, class Rep, class Period>
auto with_timeout_detached(io_executor ex, awaitable<Result> op, std::chrono::duration<Rep, Period> timeout)
  -> awaitable<Result> {
  co_return co_await detail::with_timeout_detached_impl(ex, std::move(op), timeout);
}

/// Convenience overload that uses `Stream::cancel()` on timeout.
template <class Result, class Rep, class Period, class Stream>
  requires cancellable_stream<Stream>
auto with_timeout(Stream& s, awaitable<Result> op, std::chrono::duration<Rep, Period> timeout)
  -> awaitable<Result> {
  co_return co_await with_timeout(s.get_executor(), std::move(op), timeout, [&]() { s.cancel(); });
}

/// Convenience overload for read-side operations.
///
/// If the stream supports `cancel_read()`, only the read side is cancelled on timeout.
/// Otherwise, falls back to `cancel()`.
template <class Result, class Rep, class Period, class Stream>
  requires cancellable_stream<Stream>
auto with_timeout_read(Stream& s, awaitable<Result> op, std::chrono::duration<Rep, Period> timeout)
  -> awaitable<Result> {
  if constexpr (cancel_readable_stream<Stream>) {
    co_return co_await with_timeout(s.get_executor(), std::move(op), timeout, [&]() { s.cancel_read(); });
  } else {
    co_return co_await with_timeout(s.get_executor(), std::move(op), timeout, [&]() { s.cancel(); });
  }
}

/// Convenience overload for write-side operations.
///
/// If the stream supports `cancel_write()`, only the write side is cancelled on timeout.
/// Otherwise, falls back to `cancel()`.
template <class Result, class Rep, class Period, class Stream>
  requires cancellable_stream<Stream>
auto with_timeout_write(Stream& s, awaitable<Result> op, std::chrono::duration<Rep, Period> timeout)
  -> awaitable<Result> {
  if constexpr (cancel_writable_stream<Stream>) {
    co_return co_await with_timeout(s.get_executor(), std::move(op), timeout, [&]() { s.cancel_write(); });
  } else {
    co_return co_await with_timeout(s.get_executor(), std::move(op), timeout, [&]() { s.cancel(); });
  }
}

}  // namespace iocoro::io

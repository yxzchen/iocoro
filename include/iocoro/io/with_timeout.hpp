#pragma once

#include <iocoro/assert.hpp>
#include <iocoro/awaitable.hpp>
#include <iocoro/co_spawn.hpp>
#include <iocoro/completion_token.hpp>
#include <iocoro/detail/require_io_executor.hpp>
#include <iocoro/detail/unique_function.hpp>
#include <iocoro/error.hpp>
#include <iocoro/executor.hpp>
#include <iocoro/expected.hpp>
#include <iocoro/io/stream_concepts.hpp>
#include <iocoro/io_executor.hpp>
#include <iocoro/steady_timer.hpp>
#include <iocoro/this_coro.hpp>
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

template <class A>
struct with_timeout_result;

template <class T>
struct with_timeout_result<iocoro::awaitable<T>> {
  using type = T;
};

template <class Awaitable>
using with_timeout_result_t = typename with_timeout_result<std::remove_cvref_t<Awaitable>>::type;

template <class Result>
struct with_timeout_result_traits;

template <class T>
struct with_timeout_result_traits<iocoro::expected<T, std::error_code>> {
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
struct with_timeout_result_traits<std::error_code> {
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

}  // namespace detail

/// Await an I/O awaitable with a deadline.
///
/// Semantics:
/// - **ex** is the unique anchor for timer + watcher + operation completion.
/// - The awaited operation is forced to complete back onto **ex**.
/// - When the timeout fires, this function:
///   - executes `on_timeout()` first
///   - then waits for `op` to finish (typically by being cancelled by `on_timeout()`).
template <class Awaitable, class OnTimeout>
  requires std::invocable<OnTimeout&> &&
           requires { typename detail::with_timeout_result_t<Awaitable>; }
auto with_timeout(io_executor ex, Awaitable&& op, std::chrono::steady_clock::duration timeout,
                  OnTimeout&& on_timeout) -> awaitable<detail::with_timeout_result_t<Awaitable>> {
  using result_t = detail::with_timeout_result_t<Awaitable>;
  using traits = detail::with_timeout_result_traits<result_t>;

  IOCORO_ENSURE(ex, "with_timeout: requires a non-empty io_executor");

  if (timeout <= std::chrono::steady_clock::duration::zero()) {
    on_timeout();
    auto r = co_await std::move(op);
    if (traits::is_operation_aborted(r)) {
      co_return traits::timed_out();
    }
    co_return r;
  }

  auto timer = std::make_shared<steady_timer>(ex);
  (void)timer->expires_after(std::chrono::duration_cast<steady_timer::duration>(timeout));

  std::atomic<bool> fired{false};

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

  (void)timer->cancel();
  (void)co_await std::move(watcher);

  if (fired.load(std::memory_order_acquire) && traits::is_operation_aborted(r)) {
    co_return traits::timed_out();
  }

  co_return r;
}

/// Syntax sugar 1: IO coroutine usage.
///
/// Strong requirement:
/// - The current coroutine MUST be running on io_executor.
/// - Otherwise this fails at runtime (require_io_executor).
///
/// This is purely:
/// - with_timeout(co_await this_coro::executor, ...)
/// and does not introduce any new semantics.
template <class Awaitable, class OnTimeout>
  requires std::invocable<OnTimeout&> &&
           requires { typename detail::with_timeout_result_t<Awaitable>; }
auto with_timeout(Awaitable&& op, std::chrono::steady_clock::duration timeout,
                  OnTimeout&& on_timeout) -> awaitable<detail::with_timeout_result_t<Awaitable>> {
  auto ex_any = co_await this_coro::executor;
  IOCORO_ENSURE(ex_any, "with_timeout: requires a bound executor");
  auto ex = ::iocoro::detail::require_io_executor(ex_any);
  co_return co_await with_timeout(ex, std::forward<Awaitable>(op), timeout,
                                  std::forward<OnTimeout>(on_timeout));
}

namespace detail {

template <class Stream>
concept io_executor_stream = requires(Stream& s) {
  { s.get_executor() } -> std::same_as<io_executor>;
};

}  // namespace detail

/// Syntax sugar 2: stream-bound.
///
/// Rules:
/// - executor source: s.get_executor() (must be io_executor)
/// - on_timeout is bound automatically (cancel/cancel_read/cancel_write)
template <class Stream, class Awaitable>
  requires cancellable_stream<Stream> && detail::io_executor_stream<Stream> &&
           requires { typename detail::with_timeout_result_t<Awaitable>; }
auto with_timeout(Stream& s, Awaitable&& op, std::chrono::steady_clock::duration timeout)
  -> awaitable<detail::with_timeout_result_t<Awaitable>> {
  co_return co_await with_timeout(s.get_executor(), std::forward<Awaitable>(op), timeout,
                                  [&]() { s.cancel(); });
}

template <class Stream, class Awaitable>
  requires cancellable_stream<Stream> && detail::io_executor_stream<Stream> &&
           requires { typename detail::with_timeout_result_t<Awaitable>; }
auto with_timeout_read(Stream& s, Awaitable&& op, std::chrono::steady_clock::duration timeout)
  -> awaitable<detail::with_timeout_result_t<Awaitable>> {
  if constexpr (cancel_readable_stream<Stream>) {
    co_return co_await with_timeout(s.get_executor(), std::forward<Awaitable>(op), timeout,
                                    [&]() { s.cancel_read(); });
  } else {
    co_return co_await with_timeout(s.get_executor(), std::forward<Awaitable>(op), timeout,
                                    [&]() { s.cancel(); });
  }
}

template <class Stream, class Awaitable>
  requires cancellable_stream<Stream> && detail::io_executor_stream<Stream> &&
           requires { typename detail::with_timeout_result_t<Awaitable>; }
auto with_timeout_write(Stream& s, Awaitable&& op, std::chrono::steady_clock::duration timeout)
  -> awaitable<detail::with_timeout_result_t<Awaitable>> {
  if constexpr (cancel_writable_stream<Stream>) {
    co_return co_await with_timeout(s.get_executor(), std::forward<Awaitable>(op), timeout,
                                    [&]() { s.cancel_write(); });
  } else {
    co_return co_await with_timeout(s.get_executor(), std::forward<Awaitable>(op), timeout,
                                    [&]() { s.cancel(); });
  }
}

/// Detached variant.
///
/// Semantics:
/// - On timeout, returns timed_out without waiting op to complete.
/// - op may continue running on ex after this returns.
template <class Awaitable>
  requires requires { typename detail::with_timeout_result_t<Awaitable>; }
auto with_timeout_detached(io_executor ex, Awaitable&& op,
                           std::chrono::steady_clock::duration timeout)
  -> awaitable<detail::with_timeout_result_t<Awaitable>> {
  using result_t = detail::with_timeout_result_t<Awaitable>;
  using traits = detail::with_timeout_result_traits<result_t>;

  IOCORO_ENSURE(ex, "with_timeout_detached: requires a non-empty io_executor");

  if (timeout <= std::chrono::steady_clock::duration::zero()) {
    co_return traits::timed_out();
  }

  auto timer = std::make_shared<steady_timer>(ex);
  (void)timer->expires_after(std::chrono::duration_cast<steady_timer::duration>(timeout));

  auto timer_wait = [timer]() -> awaitable<std::error_code> {
    co_return co_await timer->async_wait(use_awaitable);
  };

  // Start both concurrently; whichever finishes first determines the result.
  // NOTE: when_any does not cancel the losing task.
  auto [index, v] = co_await when_any(std::forward<Awaitable>(op), timer_wait());

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

template <class Awaitable>
  requires requires { typename detail::with_timeout_result_t<Awaitable>; }
auto with_timeout_detached(Awaitable&& op, std::chrono::steady_clock::duration timeout)
  -> awaitable<detail::with_timeout_result_t<Awaitable>> {
  auto ex_any = co_await this_coro::executor;
  IOCORO_ENSURE(ex_any, "with_timeout_detached: requires a bound executor");
  auto ex = ::iocoro::detail::require_io_executor(ex_any);
  co_return co_await with_timeout_detached(ex, std::forward<Awaitable>(op), timeout);
}

}  // namespace iocoro::io

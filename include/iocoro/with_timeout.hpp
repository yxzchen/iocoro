#pragma once

#include <iocoro/any_executor.hpp>
#include <iocoro/assert.hpp>
#include <iocoro/awaitable.hpp>
#include <iocoro/co_spawn.hpp>
#include <iocoro/completion_token.hpp>
#include <iocoro/detail/executor_cast.hpp>
#include <iocoro/detail/unique_function.hpp>
#include <iocoro/error.hpp>
#include <iocoro/expected.hpp>
#include <iocoro/io/stream_concepts.hpp>
#include <iocoro/io_executor.hpp>
#include <iocoro/steady_timer.hpp>
#include <iocoro/this_coro.hpp>
#include <iocoro/traits/awaitable_result.hpp>
#include <iocoro/traits/timeout_result.hpp>
#include <iocoro/when_any.hpp>

#include <atomic>
#include <chrono>
#include <concepts>
#include <memory>
#include <system_error>
#include <type_traits>
#include <utility>

namespace iocoro {

/// Await an I/O awaitable with a deadline.
///
/// Semantics:
/// - **ex** is the unique anchor for timer + watcher + operation completion.
/// - The awaited operation is forced to complete back onto **ex**.
/// - When the timeout fires, this function:
///   - executes `on_timeout()` first
///   - then waits for `op` to finish (typically by being cancelled by `on_timeout()`).
///
/// Note: on_timeout is taken as unique_function to avoid -Wsubobject-linkage warnings
/// when users pass lambdas with internal linkage.
template <class Awaitable>
  requires requires { typename traits::awaitable_result_t<Awaitable>; }
auto with_timeout(io_executor ex, Awaitable op, std::chrono::steady_clock::duration timeout,
                  detail::unique_function<void()> on_timeout)
  -> awaitable<traits::awaitable_result_t<Awaitable>> {
  using result_t = traits::awaitable_result_t<Awaitable>;
  using result_traits = traits::timeout_result_traits<result_t>;

  IOCORO_ENSURE(ex, "with_timeout: requires a non-empty io_executor");

  if (timeout <= std::chrono::steady_clock::duration::zero()) {
    on_timeout();
    auto r = co_await std::move(op);
    if (result_traits::is_operation_aborted(r)) {
      co_return result_traits::timed_out();
    }
    co_return r;
  }

  auto timer = std::make_shared<steady_timer>(ex);
  timer->expires_after(timeout);

  std::atomic<bool> fired{false};

  auto watcher = co_spawn(
    co_await this_coro::executor,

    // Capturing timer by value causes double free. Not sure why.
    [&timer, &fired, &on_timeout]() mutable -> awaitable<void> {
      auto ec = co_await timer->async_wait(use_awaitable);
      if (!ec) {
        fired.store(true, std::memory_order_release);
        on_timeout();
      }
    },
    use_awaitable);

  auto r = co_await std::move(op);

  timer->cancel();
  co_await std::move(watcher);

  if (fired.load(std::memory_order_acquire) && result_traits::is_operation_aborted(r)) {
    co_return result_traits::timed_out();
  }

  co_return r;
}

/// Syntax sugar: IO coroutine usage.
///
/// Strong requirement:
/// - The current coroutine MUST be running on io_executor.
/// - Otherwise this fails at runtime (require_executor).
///
/// This is purely:
/// - with_timeout(co_await this_coro::executor, ...)
/// and does not introduce any new semantics.
template <class Awaitable>
  requires requires { typename traits::awaitable_result_t<Awaitable>; }
auto with_timeout(Awaitable op, std::chrono::steady_clock::duration timeout,
                  detail::unique_function<void()> on_timeout)
  -> awaitable<traits::awaitable_result_t<Awaitable>> {
  auto ex_any = co_await this_coro::executor;
  IOCORO_ENSURE(ex_any, "with_timeout: requires a bound executor");
  auto ex = detail::require_executor<io_executor>(ex_any);
  co_return co_await with_timeout(ex, std::move(op), timeout, std::move(on_timeout));
}

/// Detached variant.
///
/// Semantics:
/// - On timeout, returns timed_out without waiting op to complete.
/// - op may continue running on ex after this returns.
template <class Awaitable>
  requires requires { typename traits::awaitable_result_t<Awaitable>; }
auto with_timeout_detached(io_executor ex, Awaitable op,
                           std::chrono::steady_clock::duration timeout)
  -> awaitable<traits::awaitable_result_t<Awaitable>> {
  using result_t = traits::awaitable_result_t<Awaitable>;
  using result_traits = traits::timeout_result_traits<result_t>;

  IOCORO_ENSURE(ex, "with_timeout_detached: requires a non-empty io_executor");

  if (timeout <= std::chrono::steady_clock::duration::zero()) {
    co_return result_traits::timed_out();
  }

  auto timer = std::make_shared<steady_timer>(ex);
  timer->expires_after(timeout);

  auto timer_wait = [timer]() -> awaitable<std::error_code> {
    co_return co_await timer->async_wait(use_awaitable);
  };

  // Start both concurrently; whichever finishes first determines the result.
  // NOTE: when_any does not cancel the losing task.
  auto [index, v] = co_await when_any(std::move(op), timer_wait());

  if (index == 0) {
    timer->cancel();
    co_return std::get<0>(std::move(v));
  }

  auto ec = std::get<1>(std::move(v));
  if (!ec) {
    co_return result_traits::timed_out();
  }

  // Timer wait completed due to cancellation/executor shutdown.
  // Treat it as a timer error rather than a timeout.
  co_return result_traits::from_error(ec);
}

template <class Awaitable>
  requires requires { typename traits::awaitable_result_t<Awaitable>; }
auto with_timeout_detached(Awaitable op, std::chrono::steady_clock::duration timeout)
  -> awaitable<traits::awaitable_result_t<Awaitable>> {
  auto ex_any = co_await this_coro::executor;
  IOCORO_ENSURE(ex_any, "with_timeout_detached: requires a bound executor");
  auto ex = detail::require_executor<io_executor>(ex_any);
  co_return co_await with_timeout_detached(ex, std::move(op), timeout);
}

}  // namespace iocoro

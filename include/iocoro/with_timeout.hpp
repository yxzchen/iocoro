#pragma once

#include <iocoro/any_executor.hpp>
#include <iocoro/assert.hpp>
#include <iocoro/awaitable.hpp>
#include <iocoro/cancellation_token.hpp>
#include <iocoro/co_spawn.hpp>
#include <iocoro/completion_token.hpp>
#include <iocoro/detail/executor_cast.hpp>
#include <iocoro/error.hpp>
#include <iocoro/expected.hpp>
#include <iocoro/io_executor.hpp>
#include <iocoro/steady_timer.hpp>
#include <iocoro/this_coro.hpp>
#include <iocoro/traits/awaitable_result.hpp>
#include <iocoro/traits/timeout_result.hpp>
#include <iocoro/when_any.hpp>

#include <chrono>
#include <concepts>
#include <memory>
#include <system_error>
#include <type_traits>
#include <utility>

namespace iocoro {

namespace detail {

template <class F>
using cancellable_awaitable_t = std::invoke_result_t<F&, cancellation_token>;

template <class F>
using cancellable_result_t = traits::awaitable_result_t<cancellable_awaitable_t<F>>;

template <class F>
concept cancellable_operation_factory =
  std::invocable<F&, cancellation_token> && requires { typename cancellable_result_t<F>; };

}  // namespace detail

/// Await an operation with a deadline.
///
/// Model:
/// - timeout is a cancellation source: when the deadline fires, we request cancellation.
/// - the operation is expected to observe cancellation_token (e.g. socket read/write/connect).
///
/// Requirements:
/// - op_factory must accept `cancellation_token` and return `iocoro::awaitable<T>`.
template <class OpFactory>
  requires detail::cancellable_operation_factory<std::remove_cvref_t<OpFactory>>
auto with_timeout(io_executor ex, OpFactory&& op_factory,
                  std::chrono::steady_clock::duration timeout)
  -> awaitable<detail::cancellable_result_t<std::remove_cvref_t<OpFactory>>> {
  using result_t = detail::cancellable_result_t<std::remove_cvref_t<OpFactory>>;
  using result_traits = traits::timeout_result_traits<result_t>;

  IOCORO_ENSURE(ex, "with_timeout: requires a non-empty io_executor");

  if (timeout <= std::chrono::steady_clock::duration::zero()) {
    co_return result_traits::timed_out();
  }

  cancellation_source timeout_src{};
  auto timeout_tok = timeout_src.token();

  auto timer = std::make_shared<steady_timer>(ex);
  timer->expires_after(timeout);

  auto timer_task = co_spawn(
    co_await this_coro::executor,
    // Capturing by value would cause heap-use-after-free. Not sure why.
    [&timer, &timeout_src]() mutable -> awaitable<void> {
      auto ec = co_await timer->async_wait(use_awaitable);
      if (!ec) {
        timeout_src.request_cancel();
      }
    },
    use_awaitable);

  auto r = co_await std::invoke(std::forward<OpFactory>(op_factory), timeout_tok);

  timer->cancel();
  co_await std::move(timer_task);

  if (timeout_tok.stop_requested() && result_traits::is_operation_aborted(r)) {
    co_return result_traits::timed_out();
  }

  co_return r;
}

/// Syntax sugar: IO coroutine usage (requires current coroutine bound to io_executor).
template <class OpFactory>
  requires detail::cancellable_operation_factory<std::remove_cvref_t<OpFactory>>
auto with_timeout(OpFactory&& op_factory, std::chrono::steady_clock::duration timeout)
  -> awaitable<detail::cancellable_result_t<std::remove_cvref_t<OpFactory>>> {
  auto ex_any = co_await this_coro::executor;
  auto ex = detail::require_executor<io_executor>(ex_any);
  co_return co_await with_timeout(ex, std::forward<OpFactory>(op_factory), timeout);
}

/// Detached variant (awaitable input).
///
/// Semantics:
/// - On timeout, returns timed_out without attempting to cancel `op`.
/// - `op` may continue running on ex after this returns.
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

  auto timer_wait = timer->async_wait(use_awaitable);

  // Start both concurrently; whichever finishes first determines the result.
  // NOTE: when_any does not cancel the losing task.
  auto [index, v] = co_await when_any(std::move(op), std::move(timer_wait));

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
  auto ex = detail::require_executor<io_executor>(ex_any);
  co_return co_await with_timeout_detached(ex, std::move(op), timeout);
}

}  // namespace iocoro

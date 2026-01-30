#pragma once

#include <iocoro/assert.hpp>
#include <iocoro/any_io_executor.hpp>
#include <iocoro/awaitable.hpp>
#include <iocoro/completion_token.hpp>
#include <iocoro/error.hpp>
#include <iocoro/expected.hpp>
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
/// Detached variant (awaitable input).
///
/// Semantics:
/// - On timeout, returns timed_out without attempting to cancel `op`.
/// - `op` may continue running on ex after this returns.
template <class Awaitable>
  requires requires { typename traits::awaitable_result_t<Awaitable>; }
auto with_timeout_detached(any_io_executor ex, Awaitable op,
                           std::chrono::steady_clock::duration timeout)
  -> awaitable<traits::awaitable_result_t<Awaitable>> {
  using result_t = traits::awaitable_result_t<Awaitable>;
  using result_traits = traits::timeout_result_traits<result_t>;

  IOCORO_ENSURE(ex, "with_timeout_detached: requires a non-empty IO executor");

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
  co_return co_await with_timeout_detached(any_io_executor{ex_any}, std::move(op), timeout);
}

}  // namespace iocoro

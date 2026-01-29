#pragma once

#include <iocoro/assert.hpp>
#include <iocoro/awaitable.hpp>
#include <iocoro/completion_token.hpp>
#include <iocoro/detail/executor_cast.hpp>
#include <iocoro/io_executor.hpp>
#include <iocoro/steady_timer.hpp>
#include <iocoro/this_coro.hpp>

#include <chrono>
#include <utility>

namespace iocoro {

/// Suspends the current coroutine for at least the given duration.
///
/// Semantics:
/// - Timer is scheduled on the provided io_executor.
/// - Completion is resumed via the timer's io_executor (never inline).
/// - If the awaiting coroutine is destroyed, the timer is implicitly cancelled.
inline auto co_sleep(io_executor ex, std::chrono::steady_clock::duration d) -> awaitable<void> {
  IOCORO_ENSURE(ex, "co_sleep: requires a non-empty io_executor");

  steady_timer t{ex};
  (void)t.expires_after(d);
  (void)co_await t.async_wait(use_awaitable);
}

inline auto co_sleep(std::chrono::steady_clock::duration d) -> awaitable<void> {
  auto ex = co_await this_coro::executor;
  co_await co_sleep(detail::require_executor<io_executor>(ex), d);
}

}  // namespace iocoro

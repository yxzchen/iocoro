#pragma once

#include <iocoro/assert.hpp>
#include <iocoro/awaitable.hpp>
#include <iocoro/completion_token.hpp>
#include <iocoro/io_executor.hpp>
#include <iocoro/steady_timer.hpp>
#include <iocoro/this_coro.hpp>

#include <chrono>
#include <utility>

namespace iocoro {

/// Suspends the current coroutine for at least the given duration.
///
/// Semantics:
/// - Timer is scheduled on the current coroutine's io_executor.
/// - Completion is resumed via the timer's io_executor (never inline).
/// - If the awaiting coroutine is destroyed, the timer is implicitly cancelled.
inline auto co_sleep(std::chrono::steady_clock::duration d) -> awaitable<void> {
  auto ex = co_await this_coro::executor;
  IOCORO_ENSURE(ex, "co_sleep: requires a bound executor");

  steady_timer t{ex};
  (void)t.expires_after(d);
  (void)co_await t.async_wait(use_awaitable);
}

}  // namespace iocoro

#pragma once

#include <iocoro/assert.hpp>
#include <iocoro/any_io_executor.hpp>
#include <iocoro/awaitable.hpp>
#include <iocoro/completion_token.hpp>
#include <iocoro/steady_timer.hpp>
#include <iocoro/this_coro.hpp>

#include <chrono>
#include <utility>

namespace iocoro {

/// Suspends the current coroutine for at least the given duration.
///
/// Semantics:
/// - Timer is scheduled on the provided IO-capable executor.
/// - Completion is resumed via the timer's executor (never inline).
inline auto co_sleep(any_io_executor ex, std::chrono::steady_clock::duration d) -> awaitable<void> {
  IOCORO_ENSURE(ex, "co_sleep: requires a non-empty IO executor");

  steady_timer t{ex};
  (void)t.expires_after(d);
  (void)co_await t.async_wait(use_awaitable);
}

inline auto co_sleep(std::chrono::steady_clock::duration d) -> awaitable<void> {
  auto ex = co_await this_coro::io_executor;
  co_await co_sleep(ex, d);
}

}  // namespace iocoro

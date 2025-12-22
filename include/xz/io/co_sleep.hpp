#pragma once

#include <xz/io/assert.hpp>
#include <xz/io/awaitable.hpp>
#include <xz/io/executor.hpp>
#include <xz/io/this_coro.hpp>
#include <xz/io/timer_handle.hpp>
#include <xz/io/use_awaitable.hpp>

#include <chrono>
#include <utility>

namespace xz::io {

/// Suspends the current coroutine for at least the given duration.
///
/// Semantics:
/// - Timer is scheduled on the current coroutine's executor.
/// - Completion is resumed via the timer's executor (never inline).
/// - If the awaiting coroutine is destroyed, the timer is implicitly cancelled.
inline auto co_sleep(std::chrono::steady_clock::duration d) -> awaitable<void> {
  auto ex = co_await this_coro::executor;
  XZ_ENSURE(ex, "co_sleep: requires a bound executor");

  using namespace std::chrono;
  auto ms = ceil<milliseconds>(d);
  if (ms.count() < 0) {
    ms = milliseconds{0};
  }

  auto th = ex.schedule_timer(ms, [] {});
  co_await th.async_wait(use_awaitable);
}

}  // namespace xz::io

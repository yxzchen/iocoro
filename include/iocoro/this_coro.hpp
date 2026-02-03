#pragma once

#include <iocoro/any_executor.hpp>
#include <stop_token>
#include <utility>

namespace iocoro::this_coro {

/// Awaitable token yielding the current coroutine's bound executor (`any_executor`).
///
/// IMPORTANT: If the current coroutine has no executor bound, awaiting this token may fail
/// via assertion/ensure inside the promise (depending on the promise type).
struct executor_t {};
inline constexpr executor_t executor{};

/// Awaitable token yielding the current coroutine's bound IO executor (`any_io_executor`).
struct io_executor_t {};
inline constexpr io_executor_t io_executor{};

/// Awaitable token yielding the current coroutine's stop token (`std::stop_token`).
struct stop_token_t {};
inline constexpr stop_token_t stop_token{};

struct switch_to_t {
  any_executor ex;
};

/// Switch the current coroutine to resume on the given executor.
///
/// Semantics:
/// - Causes the coroutine to suspend and later resume via `ex`.
/// - Best-effort "migration": this changes scheduling, not the current thread in-place.
inline auto switch_to(any_executor ex) noexcept -> switch_to_t {
  return switch_to_t{std::move(ex)};
}

struct on_t {
  any_executor ex;
};

/// Schedule the current coroutine to resume once on the given executor.
///
/// Semantics:
/// - Causes the coroutine to suspend and later resume via `ex` (one-shot).
/// - Does NOT rebind the coroutine's long-term executor (promise binding).
/// - Only affects the next resumption; subsequent awaits decide their own scheduling.
inline auto on(any_executor ex) noexcept -> on_t { return on_t{std::move(ex)}; }

}  // namespace iocoro::this_coro

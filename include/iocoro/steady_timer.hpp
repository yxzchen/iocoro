#pragma once

#include <iocoro/awaitable.hpp>
#include <iocoro/completion_token.hpp>
#include <iocoro/io_executor.hpp>
#include <iocoro/timer_handle.hpp>

#include <chrono>
#include <cstddef>
#include <functional>
#include <system_error>

namespace iocoro {

/// A reusable timer resource bound to an io_executor.
///
/// Model:
/// - Owns a "timer resource".
/// - You can set expiry (expires_at/after), then wait (async_wait).
/// - cancel() cancels the underlying timer and completes pending waits.
///
/// Notes:
/// - Normal case: completion handlers / coroutine resumption are scheduled via the bound io_executor
///   (never inline).
/// - Exception: if the bound io_executor is stopped, operations may complete inline to avoid hanging.
class steady_timer {
 public:
  using clock = std::chrono::steady_clock;
  using time_point = clock::time_point;
  using duration = clock::duration;

  explicit steady_timer(io_executor ex) noexcept;
  steady_timer(io_executor ex, time_point at) noexcept;
  steady_timer(io_executor ex, duration after) noexcept;

  steady_timer(steady_timer const&) = delete;
  auto operator=(steady_timer const&) -> steady_timer& = delete;
  steady_timer(steady_timer&&) = delete;
  auto operator=(steady_timer&&) -> steady_timer& = delete;

  ~steady_timer();

  auto get_executor() const noexcept -> io_executor { return ex_; }

  auto expiry() const noexcept -> time_point { return expiry_; }
  /// Set the timer expiry time.
  ///
  /// Returns the number of pending waits that were cancelled (best-effort).
  auto expires_at(time_point at) noexcept -> std::size_t;

  /// Set the timer expiry time relative to now.
  ///
  /// Returns the number of pending waits that were cancelled (best-effort).
  auto expires_after(duration d) noexcept -> std::size_t;

  /// Wait until expiry (or cancellation) and invoke handler.
  ///
  /// Handler signature: void(std::error_code)
  ///
  /// Completion semantics:
  /// - Normal case: handler is invoked via the timer's owning io_executor (never inline).
  /// - Exception: if the owning io_executor is stopped (or handle is empty), completion may be inline.
  /// - `ec == operation_aborted` iff the timer was cancelled.
  void async_wait(std::function<void(std::error_code)> h);

  /// Wait until expiry (or cancellation) as an awaitable.
  ///
  /// Returns:
  /// - `ec == operation_aborted` iff the timer was cancelled.
  ///
  /// Completion semantics:
  /// - Normal case: coroutine resumption occurs via the timer's owning io_executor (never inline).
  /// - Exception: if the owning io_executor is stopped (or handle is empty), it completes inline.
  auto async_wait(use_awaitable_t) -> awaitable<std::error_code>;

  /// Cancel pending waits on the timer (best-effort).
  ///
  /// Returns the number of pending waits that were cancelled (best-effort).
  auto cancel() noexcept -> std::size_t;

 private:
  void reschedule();

  io_executor ex_{};
  time_point expiry_{clock::now()};
  timer_handle th_{};
};

}  // namespace iocoro

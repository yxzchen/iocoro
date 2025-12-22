#pragma once

#include <cstddef>
#include <functional>
#include <memory>

namespace xz::io {

class executor;
class steady_timer;

namespace detail {
struct timer_entry;
}  // namespace detail

/// A lightweight, copyable handle to a scheduled timer.
/// Multiple handles can reference the same timer and any of them can cancel it.
/// The timer is kept alive as long as at least one handle or the io_context holds a reference.
class timer_handle {
 public:
  timer_handle() noexcept = default;

  /// Attempts to cancel the timer.
  ///
  /// Returns the number of pending waits that were cancelled (best-effort).
  /// Returns 0 if the timer was already fired/cancelled, or the handle is empty.
  auto cancel() const noexcept -> std::size_t;

  /// Returns true if the timer is still pending (not fired or cancelled).
  auto pending() const noexcept -> bool;

  /// Returns true if the timer has fired.
  auto fired() const noexcept -> bool;

  /// Returns true if the timer has been cancelled.
  auto cancelled() const noexcept -> bool;

  explicit operator bool() const noexcept;

 private:
  friend class executor;
  friend class steady_timer;

  /// Private constructor for executor to create handles.
  explicit timer_handle(std::shared_ptr<detail::timer_entry> entry) noexcept;

  // steady_timer hook: add a completion waiter without exposing timer_entry.
  void add_waiter(std::function<void()> w) const;

  std::shared_ptr<detail::timer_entry> entry_;
};

}  // namespace xz::io

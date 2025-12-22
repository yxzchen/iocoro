#pragma once

#include <xz/io/awaitable.hpp>
#include <xz/io/use_awaitable.hpp>

#include <memory>

namespace xz::io {

class io_context;

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
  /// Returns true if the timer was pending and is now cancelled.
  /// Returns false if the timer was already fired, cancelled, or the handle is empty.
  auto cancel() noexcept -> bool;

  /// Returns true if the timer is still pending (not fired or cancelled).
  auto valid() const noexcept -> bool;

  explicit operator bool() const noexcept;

  /// Await timer completion (fired or cancelled) as an awaitable.
  ///
  /// Semantics:
  /// - Resumption is scheduled via the timer's executor (never inline).
  /// - Destroying the awaiting coroutine implicitly cancels the timer.
  auto async_wait(use_awaitable_t) -> awaitable<void>;

 private:
  friend class executor;

  /// Private constructor for io_context to create handles.
  explicit timer_handle(std::shared_ptr<detail::timer_entry> entry) noexcept;

  std::shared_ptr<detail::timer_entry> entry_;
};

}  // namespace xz::io

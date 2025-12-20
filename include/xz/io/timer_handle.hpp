#pragma once

#include <xz/io/io_context.hpp>

namespace xz::io {

namespace detail {
  class timer_entry;
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
  auto cancel() noexcept -> bool {
    if (!entry_) {
      return false;
    }
    return entry_->cancel();
  }

  /// Returns true if the timer is still pending (not fired or cancelled).
  auto valid() const noexcept -> bool { return entry_ && entry_->is_pending(); }

  explicit operator bool() const noexcept { return entry_ != nullptr; }

 private:
  friend class io_context;

  /// Private constructor for io_context to create handles.
  explicit timer_handle(std::shared_ptr<detail::timer_entry> entry) noexcept
      : entry_(std::move(entry)) {}

  std::shared_ptr<detail::timer_entry> entry_;
};

}  // namespace xz::io

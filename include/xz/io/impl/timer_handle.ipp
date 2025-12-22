#pragma once

#include <xz/io/detail/timer_entry.hpp>
#include <xz/io/timer_handle.hpp>

#include <utility>

namespace xz::io {

auto timer_handle::cancel() noexcept -> bool {
  if (!entry_) {
    return false;
  }
  auto const cancelled = entry_->cancel();
  if (cancelled) {
    // Ensure awaiters observing this timer are completed (via posted work).
    entry_->notify_completion();
  }
  return cancelled;
}

auto timer_handle::pending() const noexcept -> bool { return entry_ && entry_->is_pending(); }

auto timer_handle::fired() const noexcept -> bool { return entry_ && entry_->is_fired(); }

auto timer_handle::cancelled() const noexcept -> bool { return entry_ && entry_->is_cancelled(); }

timer_handle::operator bool() const noexcept { return entry_ != nullptr; }

timer_handle::timer_handle(std::shared_ptr<detail::timer_entry> entry) noexcept
    : entry_(std::move(entry)) {}

}  // namespace xz::io

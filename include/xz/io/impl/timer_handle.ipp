#pragma once

#include <xz/io/detail/timer_entry.hpp>
#include <xz/io/timer_handle.hpp>

#include <utility>

namespace xz::io {

auto timer_handle::cancel() noexcept -> bool {
  if (!entry_) {
    return false;
  }
  return entry_->cancel();
}

auto timer_handle::valid() const noexcept -> bool { return entry_ && entry_->is_pending(); }

timer_handle::operator bool() const noexcept { return entry_ != nullptr; }

timer_handle::timer_handle(std::shared_ptr<detail::timer_entry> entry) noexcept
    : entry_(std::move(entry)) {}

}  // namespace xz::io

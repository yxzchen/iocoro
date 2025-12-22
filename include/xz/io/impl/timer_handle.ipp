#include <xz/io/detail/timer_entry.hpp>
#include <xz/io/timer_handle.hpp>

#include <utility>

namespace xz::io {

inline auto timer_handle::cancel() const noexcept -> std::size_t {
  if (!entry_) {
    return 0;
  }
  auto const cancelled = entry_->cancel();
  if (!cancelled) {
    return 0;
  }
  // Ensure awaiters observing this timer are completed (via posted work).
  try {
    return entry_->notify_completion();
  } catch (...) {
    // Best-effort: timer_handle::cancel() is noexcept.
    return 0;
  }
}

inline auto timer_handle::pending() const noexcept -> bool {
  return entry_ && entry_->is_pending();
}

inline auto timer_handle::fired() const noexcept -> bool { return entry_ && entry_->is_fired(); }

inline auto timer_handle::cancelled() const noexcept -> bool {
  return entry_ && entry_->is_cancelled();
}

timer_handle::operator bool() const noexcept { return entry_ != nullptr; }

timer_handle::timer_handle(std::shared_ptr<detail::timer_entry> entry) noexcept
    : entry_(std::move(entry)) {}

inline void timer_handle::add_waiter(std::function<void()> w) const {
  if (!entry_) {
    return;
  }
  entry_->add_waiter(std::move(w));
}

}  // namespace xz::io

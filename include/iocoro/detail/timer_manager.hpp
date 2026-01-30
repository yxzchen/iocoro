#pragma once

#include <iocoro/detail/reactor_types.hpp>
#include <iocoro/detail/timer_entry.hpp>
#include <iocoro/error.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <utility>

namespace iocoro::detail {

struct timer_entry_compare {
  auto operator()(const std::shared_ptr<timer_entry>& lhs,
                  const std::shared_ptr<timer_entry>& rhs) const noexcept -> bool {
    return lhs->expiry > rhs->expiry;
  }
};

class timer_manager {
 public:
  auto add_timer(std::chrono::steady_clock::time_point expiry, reactor_op_ptr op)
    -> std::shared_ptr<timer_entry>;
  auto cancel(timer_event_handle h) noexcept -> bool;
  auto next_timeout() const -> std::optional<std::chrono::milliseconds>;
  auto process_expired(bool stopped) -> std::size_t;
  auto empty() const -> bool;

 private:
  mutable std::mutex mtx_{};
  std::priority_queue<std::shared_ptr<timer_entry>,
                      std::vector<std::shared_ptr<timer_entry>>,
                      timer_entry_compare>
    timers_{};
  std::uint64_t next_timer_id_ = 1;
};

inline auto timer_manager::add_timer(std::chrono::steady_clock::time_point expiry,
                                     reactor_op_ptr op) -> std::shared_ptr<timer_entry> {
  auto entry = std::make_shared<detail::timer_entry>();
  entry->expiry = expiry;
  entry->op = std::move(op);
  entry->state.store(timer_state::pending, std::memory_order_release);

  {
    std::scoped_lock lk{mtx_};
    entry->id = next_timer_id_++;
    timers_.push(entry);
  }

  return entry;
}

inline auto timer_manager::cancel(timer_event_handle h) noexcept -> bool {
  if (!h) {
    return false;
  }
  return h.entry->cancel();
}

inline auto timer_manager::next_timeout() const -> std::optional<std::chrono::milliseconds> {
  std::scoped_lock lk{mtx_};

  if (timers_.empty()) {
    return std::nullopt;
  }

  if (timers_.top()->is_cancelled()) {
    return std::chrono::milliseconds(0);
  }

  auto const now = std::chrono::steady_clock::now();
  auto const expiry = timers_.top()->expiry;
  if (expiry <= now) {
    return std::chrono::milliseconds(0);
  }
  return std::chrono::duration_cast<std::chrono::milliseconds>(expiry - now);
}

inline auto timer_manager::process_expired(bool stopped) -> std::size_t {
  std::unique_lock lk{mtx_};
  auto const now = std::chrono::steady_clock::now();
  std::size_t count = 0;

  while (!timers_.empty()) {
    if (stopped) {
      break;
    }

    auto entry = timers_.top();

    if (entry->is_cancelled()) {
      timers_.pop();
      auto op = std::move(entry->op);
      lk.unlock();
      if (op) {
        op->vt->on_abort(op->block, error::operation_aborted);
      }
      lk.lock();
      continue;
    }

    if (entry->expiry > now) {
      break;
    }

    timers_.pop();

    if (!entry->mark_fired()) {
      continue;
    }

    auto op = std::move(entry->op);
    lk.unlock();
    if (op) {
      op->vt->on_complete(op->block);
    }

    ++count;
    lk.lock();
  }

  return count;
}

inline auto timer_manager::empty() const -> bool {
  std::scoped_lock lk{mtx_};
  return timers_.empty();
}

}  // namespace iocoro::detail

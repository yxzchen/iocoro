#pragma once

#include <iocoro/assert.hpp>
#include <iocoro/executor.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace iocoro::detail {

enum class timer_state : std::uint8_t {
  pending,
  fired,
  cancelled,
};

/// Internal timer data structure.
/// Shared between timer_handle and io_context_impl via shared_ptr.
struct timer_entry {
  // NOTE: fields in this struct follow a write-once then read-many pattern.
  // The owning io_context_impl initializes id/expiry/callback/ex before publishing
  // the shared_ptr to other threads. After publication, these fields are treated
  // as immutable (except for state + waiter bookkeeping).

  std::uint64_t id{};
  std::chrono::steady_clock::time_point expiry;
  std::function<void()> callback;

  // Executor that owns this timer (context thread affinity).
  iocoro::executor ex{};

  std::atomic<timer_state> state{timer_state::pending};

  // Waiters are completion hooks (e.g. async_wait(use_awaitable)).
  // They should never resume inline; they are expected to post/dispatch as needed.
  bool completion_notified = false;
  std::mutex waiters_mutex{};
  std::vector<std::function<void()>> waiters{};

  // Constructors
  timer_entry() = default;

  timer_entry(const timer_entry&) = delete;
  auto operator=(const timer_entry&) -> timer_entry& = delete;
  timer_entry(timer_entry&&) = delete;
  auto operator=(timer_entry&&) -> timer_entry& = delete;

  auto is_pending() const noexcept -> bool {
    return state.load(std::memory_order_acquire) == timer_state::pending;
  }

  auto is_fired() const noexcept -> bool {
    return state.load(std::memory_order_acquire) == timer_state::fired;
  }

  auto is_cancelled() const noexcept -> bool {
    return state.load(std::memory_order_acquire) == timer_state::cancelled;
  }

  auto mark_fired() noexcept -> bool {
    auto expected = timer_state::pending;
    return state.compare_exchange_strong(expected, timer_state::fired, std::memory_order_acq_rel,
                                         std::memory_order_acquire);
  }

  auto cancel() noexcept -> bool {
    auto expected = timer_state::pending;
    return state.compare_exchange_strong(expected, timer_state::cancelled,
                                         std::memory_order_acq_rel, std::memory_order_acquire);
  }

  void post_waiter(std::function<void()> w) {
    IOCORO_ENSURE(ex, "timer_entry: missing executor for waiter dispatch");
    // Never execute waiters inline: always schedule via the owning executor.
    ex.post([w = std::move(w)]() mutable { w(); });
  }

  void add_waiter(std::function<void()> w) {
    if (!w) {
      return;
    }

    bool post_now = false;
    {
      std::scoped_lock lk{waiters_mutex};
      if (completion_notified) {
        post_now = true;
      } else {
        waiters.push_back(std::move(w));
      }
    }

    if (post_now) {
      post_waiter(std::move(w));
    }
  }

  auto notify_completion() -> std::size_t {
    std::vector<std::function<void()>> local;
    {
      std::scoped_lock lk{waiters_mutex};
      if (completion_notified) return 0;
      completion_notified = true;
      local.swap(waiters);
    }

    if (local.empty()) {
      return 0;
    }

    IOCORO_ENSURE(ex, "timer_entry: missing executor for completion notification");
    // Never execute waiters inline: always schedule via the owning executor.
    auto const n = local.size();
    ex.post([local = std::move(local)]() mutable {
      for (auto& w : local) {
        w();
      }
    });
    return n;
  }
};

}  // namespace iocoro::detail

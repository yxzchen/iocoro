#pragma once

#include <iocoro/detail/unique_function.hpp>
#include <iocoro/detail/work_guard_counter.hpp>

#include <atomic>
#include <mutex>
#include <queue>
#include <utility>

namespace iocoro::detail {

// Cross-thread queue for tasks posted into an `io_context_impl`.
//
// Design constraints:
// - `post()` may be called from any thread.
// - Draining (`process()`) happens on the reactor thread.
// - Fairness: draining is bounded per tick to avoid starving timers/IO.
class posted_queue {
 public:
  static constexpr std::size_t max_drain_per_tick = 256;

  void post(unique_function<void()> f) {
    std::scoped_lock lk{mtx_};
    queue_.push(std::move(f));
  }

  auto process(bool stopped) -> std::size_t {
    std::queue<unique_function<void()>> local;
    {
      std::scoped_lock lk{mtx_};
      std::swap(local, queue_);
    }

    if (local.empty()) {
      return 0;
    }

    std::size_t n = 0;
    while (!local.empty()) {
      if (stopped) {
        // IMPORTANT: When the event loop is stopped we do not execute user callbacks.
        // Preserve posted tasks so that `restart()` can resume from a consistent state.
        std::scoped_lock lk{mtx_};
        while (!local.empty()) {
          queue_.push(std::move(local.front()));
          local.pop();
        }
        break;
      }

      auto f = std::move(local.front());
      local.pop();
      if (f) {
        f();
      }
      ++n;

      // Fairness: do not drain indefinitely; allow timers/IO to run.
      if (n >= max_drain_per_tick) {
        break;
      }
    }

    if (!stopped && !local.empty()) {
      std::scoped_lock lk{mtx_};
      while (!local.empty()) {
        queue_.push(std::move(local.front()));
        local.pop();
      }
    }
    return n;
  }

  void add_work_guard() noexcept { work_guard_.add(); }

  auto remove_work_guard() noexcept -> std::size_t {
    return work_guard_.remove();
  }

  auto work_guard_count() const noexcept -> std::size_t {
    return work_guard_.count();
  }

  auto has_work() const -> bool {
    if (work_guard_.has_work()) {
      return true;
    }
    std::scoped_lock lk{mtx_};
    return !queue_.empty();
  }

  // True iff there are queued posted tasks (does NOT consider work_guard).
  auto has_pending_tasks() const -> bool {
    std::scoped_lock lk{mtx_};
    return !queue_.empty();
  }

 private:
  mutable std::mutex mtx_{};
  std::queue<unique_function<void()>> queue_{};
  work_guard_counter work_guard_{};
};

}  // namespace iocoro::detail

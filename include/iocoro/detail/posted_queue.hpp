#pragma once

#include <iocoro/detail/unique_function.hpp>

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
class posted_queue {
 public:
  void post(unique_function<void()> f) {
    std::scoped_lock lk{mtx_};
    queue_.push(std::move(f));
    pending_count_.fetch_add(1, std::memory_order_release);
  }

  auto process() -> std::size_t {
    std::queue<unique_function<void()>> local;
    {
      std::scoped_lock lk{mtx_};
      std::swap(local, queue_);
    }

    if (local.size() > 0) {
      (void)pending_count_.fetch_sub(local.size(), std::memory_order_acq_rel);
    }

    if (local.empty()) {
      return 0;
    }

    std::size_t n = 0;
    while (!local.empty()) {
      auto f = std::move(local.front());
      local.pop();
      if (f) {
        f();
      }
      ++n;
    }

    return n;
  }

  // True iff there are queued posted tasks.
  auto has_pending_tasks() const -> bool {
    return pending_count_.load(std::memory_order_acquire) > 0;
  }

 private:
  mutable std::mutex mtx_{};
  std::queue<unique_function<void()>> queue_{};
  std::atomic<std::size_t> pending_count_{0};
};

}  // namespace iocoro::detail

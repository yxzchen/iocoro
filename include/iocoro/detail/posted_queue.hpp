#pragma once

#include <iocoro/assert.hpp>
#include <iocoro/detail/unique_function.hpp>

#include <atomic>
#include <mutex>
#include <queue>
#include <utility>

namespace iocoro::detail {

class posted_queue {
 public:
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
    }
    return n;
  }

  void add_work_guard() noexcept { work_guard_.fetch_add(1, std::memory_order_acq_rel); }

  auto remove_work_guard() noexcept -> std::size_t {
    auto const old = work_guard_.fetch_sub(1, std::memory_order_acq_rel);
    IOCORO_ENSURE(old > 0, "posted_queue: remove_work_guard() without add_work_guard()");
    return old;
  }

  auto work_guard_count() const noexcept -> std::size_t {
    return work_guard_.load(std::memory_order_acquire);
  }

  auto has_work() const -> bool {
    if (work_guard_count() > 0) {
      return true;
    }
    std::scoped_lock lk{mtx_};
    return !queue_.empty();
  }

 private:
  mutable std::mutex mtx_{};
  std::queue<unique_function<void()>> queue_{};
  std::atomic<std::size_t> work_guard_{0};
};

}  // namespace iocoro::detail

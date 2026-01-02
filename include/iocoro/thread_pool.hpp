#pragma once

#include <iocoro/any_executor.hpp>
#include <iocoro/assert.hpp>
#include <iocoro/detail/executor_guard.hpp>
#include <iocoro/detail/unique_function.hpp>
#include <iocoro/work_guard.hpp>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <queue>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace iocoro {

/// A simple thread pool with a shared task queue.
///
/// Design:
/// - Maintains a single shared task queue protected by a mutex.
/// - Starts N worker threads that all pull tasks from the shared queue.
/// - Provides automatic load balancing across available threads.
/// - Executors hold shared ownership of pool state for lifetime safety.
///
/// Notes:
/// - This is intentionally minimal and does not attempt to provide advanced scheduling
///   policies. It is primarily a building block for higher-level executors.
class thread_pool {
 public:
  class basic_executor_type;
  typedef basic_executor_type executor_type;

  explicit thread_pool(std::size_t n_threads);

  thread_pool(thread_pool const&) = delete;
  auto operator=(thread_pool const&) -> thread_pool& = delete;
  thread_pool(thread_pool&&) = delete;
  auto operator=(thread_pool&&) -> thread_pool& = delete;

  ~thread_pool();

  auto get_executor() noexcept -> executor_type;

  /// Stop all worker threads (best-effort, idempotent).
  void stop() noexcept;

  /// Join all worker threads (best-effort, idempotent).
  void join() noexcept;

  auto size() const noexcept -> std::size_t;

 private:
  // Shared state that outlives the pool object
  struct state {
    mutable std::mutex mutex;
    std::condition_variable cv;
    std::queue<detail::unique_function<void()>> tasks;

    std::atomic<bool> stopped{false};
    std::atomic<std::size_t> work_guard_count{0};

    std::size_t n_threads;
  };

  std::shared_ptr<state> state_;
  std::vector<std::thread> threads_;

  // Helper methods
  void worker_loop();
};

/// A lightweight executor that schedules work onto a thread_pool.
///
/// Holds shared ownership of pool state for lifetime safety.
class thread_pool::basic_executor_type {
 public:
  basic_executor_type() noexcept = default;
  explicit basic_executor_type(std::shared_ptr<state> s) noexcept : state_(std::move(s)) {}

  basic_executor_type(basic_executor_type const&) noexcept = default;
  auto operator=(basic_executor_type const&) noexcept -> basic_executor_type& = default;
  basic_executor_type(basic_executor_type&&) noexcept = default;
  auto operator=(basic_executor_type&&) noexcept -> basic_executor_type& = default;

  template <class F>
    requires std::is_invocable_v<F&>
  void post(F&& f) const noexcept {
    if (!state_) {
      return;
    }

    {
      std::scoped_lock lock{state_->mutex};

      // Reject new tasks if stopped
      if (state_->stopped.load(std::memory_order_acquire)) {
        return;
      }

      state_->tasks.emplace(std::forward<F>(f));
    }

    state_->cv.notify_one();
  }

  template <class F>
    requires std::is_invocable_v<F&>
  void dispatch(F&& f) const noexcept {
    // TODO: Check executor equivalence via detail::current_executor()
    // For now, always post
    post(std::forward<F>(f));
  }

  auto pick_executor() const -> executor_type {
    return *this;
  }

  auto stopped() const noexcept -> bool {
    return !state_ || state_->stopped.load(std::memory_order_acquire);
  }

  explicit operator bool() const noexcept { return state_ != nullptr; }

  // Executor equality for dispatch inline判断
  auto equals(basic_executor_type const& other) const noexcept -> bool {
    return state_.get() == other.state_.get();
  }

 private:
  friend class work_guard<basic_executor_type>;

  void add_work_guard() const noexcept {
    if (state_) {
      state_->work_guard_count.fetch_add(1, std::memory_order_acq_rel);
    }
  }

  void remove_work_guard() const noexcept {
    if (state_) {
      auto old = state_->work_guard_count.fetch_sub(1, std::memory_order_acq_rel);
      IOCORO_ENSURE(old > 0, "remove_work_guard without add_work_guard");
      if (old == 1) {
        // Last guard removed, wake threads
        state_->cv.notify_all();
      }
    }
  }

  std::shared_ptr<thread_pool::state> state_;
};

}  // namespace iocoro

#include <iocoro/impl/thread_pool.ipp>

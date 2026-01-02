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

  auto size() const noexcept -> std::size_t { return n_threads_; }

 private:
  // Shared task queue
  mutable std::mutex queue_mutex_;
  std::condition_variable queue_cv_;
  std::queue<detail::unique_function<void()>> tasks_;

  // Thread pool
  std::vector<std::thread> threads_;
  std::size_t n_threads_;

  // Control flags
  std::atomic<bool> stopped_{false};

  // Work guard support (prevents premature shutdown)
  std::atomic<std::size_t> work_guard_counter_{0};

  // Thread identification for dispatch()
  static thread_local std::size_t thread_id_;

  // Helper methods
  void worker_loop(std::size_t tid);
  auto running_in_pool_thread() const noexcept -> bool;
};

/// A lightweight executor that schedules work onto a thread_pool.
///
/// This is a non-owning handle: the referenced thread_pool must outlive this object.
class thread_pool::basic_executor_type {
 public:
  basic_executor_type() noexcept = default;
  explicit basic_executor_type(thread_pool& pool) noexcept : pool_(&pool) {}

  basic_executor_type(basic_executor_type const&) noexcept = default;
  auto operator=(basic_executor_type const&) noexcept -> basic_executor_type& = default;
  basic_executor_type(basic_executor_type&&) noexcept = default;
  auto operator=(basic_executor_type&&) noexcept -> basic_executor_type& = default;

  template <class F>
    requires std::is_invocable_v<F&>
  void post(F&& f) const noexcept {
    IOCORO_ENSURE(pool_ != nullptr, "thread_pool::executor: empty pool_");

    {
      std::scoped_lock lock{pool_->queue_mutex_};
      pool_->tasks_.emplace([ex = *this, f = std::forward<F>(f)]() mutable {
        // Set up executor context for coroutines
        detail::executor_guard g{any_executor{ex}};
        f();
      });
    }

    pool_->queue_cv_.notify_one();
  }

  template <class F>
    requires std::is_invocable_v<F&>
  void dispatch(F&& f) const noexcept {
    IOCORO_ENSURE(pool_ != nullptr, "thread_pool::executor: empty pool_");

    if (pool_->running_in_pool_thread()) {
      // We're on a pool thread, execute inline
      detail::executor_guard g{any_executor{*this}};
      f();
    } else {
      // Not on pool thread, must post
      post(std::forward<F>(f));
    }
  }

  auto stopped() const noexcept -> bool {
    return pool_ == nullptr || pool_->stopped_.load(std::memory_order_acquire);
  }

  explicit operator bool() const noexcept { return pool_ != nullptr; }

 private:
  friend class work_guard<basic_executor_type>;

  void add_work_guard() const noexcept {
    if (pool_) {
      pool_->work_guard_counter_.fetch_add(1, std::memory_order_acq_rel);
    }
  }

  void remove_work_guard() const noexcept {
    if (pool_) {
      auto old = pool_->work_guard_counter_.fetch_sub(1, std::memory_order_acq_rel);
      IOCORO_ENSURE(old > 0, "remove_work_guard without add_work_guard");
      if (old == 1) {
        // Last guard removed, wake threads so they can exit if no work
        pool_->queue_cv_.notify_all();
      }
    }
  }

  thread_pool* pool_ = nullptr;
};

}  // namespace iocoro

#include <iocoro/impl/thread_pool.ipp>

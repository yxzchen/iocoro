#pragma once

#include <iocoro/any_executor.hpp>
#include <iocoro/assert.hpp>
#include <iocoro/detail/executor_cast.hpp>
#include <iocoro/detail/executor_guard.hpp>
#include <iocoro/detail/unique_function.hpp>
#include <iocoro/detail/work_guard_counter.hpp>
#include <iocoro/work_guard.hpp>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace iocoro {

/// Minimal thread pool with a shared FIFO task queue.
///
/// Semantics:
/// - `post()` schedules work onto one of the worker threads.
/// - `dispatch()` runs inline only when already executing on the same pool executor;
///   otherwise it falls back to `post()`.
///
/// This is intentionally small and acts as a building block for higher-level executors.
class thread_pool {
 public:
  class executor_type;

  /// Handler invoked when a task throws on a worker thread.
  using exception_handler_t = std::function<void(std::exception_ptr)>;

  explicit thread_pool(std::size_t n_threads);

  thread_pool(thread_pool const&) = delete;
  auto operator=(thread_pool const&) -> thread_pool& = delete;
  thread_pool(thread_pool&&) = delete;
  auto operator=(thread_pool&&) -> thread_pool& = delete;

  ~thread_pool() noexcept;

  auto get_executor() noexcept -> executor_type;

  /// Stop accepting new tasks and wake workers (idempotent).
  ///
  /// IMPORTANT: already-queued tasks are not guaranteed to run after `stop()`.
  void stop() noexcept;

  /// Join all worker threads (idempotent).
  void join() noexcept;

  auto size() const noexcept -> std::size_t;

  /// Set the handler for task exceptions (called on the worker thread).
  ///
  /// If unset, task exceptions are swallowed (detached semantics).
  void set_exception_handler(exception_handler_t handler) noexcept;

 private:
  enum class state_t : std::uint8_t { running, draining, stopped };

  mutable std::mutex cv_mutex_;
  std::condition_variable cv_;
  std::deque<detail::unique_function<void()>> queue_;

  state_t state_{state_t::running};
  detail::work_guard_counter work_guard_;

  std::size_t n_threads_{0};
  std::atomic<std::shared_ptr<exception_handler_t>> on_task_exception_{};

  std::vector<std::thread> threads_;

  static void worker_loop(thread_pool* pool, std::size_t index);
};

/// A lightweight executor that schedules work onto a thread_pool.
///
/// Holds shared ownership of pool state for lifetime safety.
class thread_pool::executor_type {
 public:
  executor_type() noexcept = default;
  explicit executor_type(thread_pool* pool) noexcept : pool_(pool) {}

  executor_type(executor_type const&) noexcept = default;
  auto operator=(executor_type const&) noexcept -> executor_type& = default;
  executor_type(executor_type&&) noexcept = default;
  auto operator=(executor_type&&) noexcept -> executor_type& = default;

  void post(detail::unique_function<void()> f) const noexcept {
    if (!pool_) {
      return;
    }

    auto task = detail::unique_function<void()>{[ex = *this, fn = std::move(f)]() mutable {
      detail::executor_guard g{any_executor{ex}};
      fn();
    }};
    {
      std::scoped_lock lock{pool_->cv_mutex_};
      if (pool_->state_ != state_t::running) {
        return;
      }
      pool_->queue_.emplace_back(std::move(task));
    }
    pool_->cv_.notify_one();
  }

  void dispatch(detail::unique_function<void()> f) const noexcept {
    if (!pool_) {
      return;
    }

    auto const cur_any = detail::get_current_executor();
    if (cur_any) {
      auto const* cur = detail::any_executor_access::target<executor_type>(cur_any);
      if (cur != nullptr && (*cur == *this)) {
        f();
        return;
      }
    }

    post(std::move(f));
  }

  /// True if the pool has been stopped (or this executor is empty).
  auto stopped() const noexcept -> bool {
    if (!pool_) {
      return true;
    }
    std::scoped_lock lock{pool_->cv_mutex_};
    return pool_->state_ != state_t::running;
  }

  explicit operator bool() const noexcept { return pool_ != nullptr; }

  friend auto operator==(executor_type const& a, executor_type const& b) noexcept -> bool {
    return a.pool_ == b.pool_;
  }
  friend auto operator!=(executor_type const& a, executor_type const& b) noexcept -> bool {
    return !(a == b);
  }

 private:
  friend class work_guard<executor_type>;

  void add_work_guard() const noexcept {
    if (pool_) {
      pool_->work_guard_.add();
    }
  }

  void remove_work_guard() const noexcept {
    if (pool_) {
      auto const old = pool_->work_guard_.remove();
      if (old == 1) {
        pool_->cv_.notify_all();
      }
    }
  }

  thread_pool* pool_{nullptr};
};

}  // namespace iocoro

namespace iocoro::detail {

template <>
struct executor_traits<thread_pool::executor_type> {
  static auto capabilities(thread_pool::executor_type const& ex) noexcept -> executor_capability {
    (void)ex;
    return executor_capability::none;
  }

  static auto io_context(thread_pool::executor_type const&) noexcept -> io_context_impl* {
    return nullptr;
  }
};

}  // namespace iocoro::detail

#include <iocoro/impl/thread_pool.ipp>

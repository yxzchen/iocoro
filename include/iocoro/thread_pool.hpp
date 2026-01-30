#pragma once

#include <iocoro/any_executor.hpp>
#include <iocoro/assert.hpp>
#include <iocoro/detail/executor_cast.hpp>
#include <iocoro/detail/executor_guard.hpp>
#include <iocoro/detail/unique_function.hpp>
#include <iocoro/work_guard.hpp>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <exception>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace iocoro {

namespace detail {
struct thread_pool_tls_state {
  void const* current_state{};
  std::vector<unique_function<void()>> deferred{};
};

inline thread_local thread_pool_tls_state thread_pool_tls{};
}  // namespace detail

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

  /// Type for exception handler callback
  using exception_handler_t = std::function<void(std::exception_ptr)>;

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

  /// Set exception handler for tasks that throw exceptions.
  /// If not set, exceptions are silently swallowed.
  /// The handler is called on the worker thread where the exception occurred.
  void set_exception_handler(exception_handler_t handler) noexcept;

 private:
  // Shared state that outlives the pool object
  struct state {
    mutable std::mutex mutex;
    std::condition_variable cv;
    std::queue<detail::unique_function<void()>> tasks;

    std::atomic<bool> stopped{false};
    std::atomic<std::size_t> work_guard_count{0};

    std::size_t n_threads;

    // Exception handler (protected by mutex since it can be set at runtime)
    exception_handler_t on_task_exception;
  };

  std::shared_ptr<state> state_;
  std::vector<std::thread> threads_;

  // Helper methods
  static void worker_loop(std::shared_ptr<state> s);
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

    // If we're already running on this pool, defer until the current task
    // returns to the worker loop. This prevents destroying coroutine frames
    // while `final_suspend().await_suspend()` is still executing on-stack.
    if (detail::thread_pool_tls.current_state == state_.get()) {
      detail::thread_pool_tls.deferred.emplace_back([ex = *this, fn = std::forward<F>(f)]() mutable {
        detail::executor_guard g{any_executor{ex}};
        fn();
      });
      return;
    }

    {
      std::scoped_lock lock{state_->mutex};

      // Post never runs inline; even during/after stop it queues work so that
      // reactor/coroutine finalization does not destroy frames on the caller stack.
      state_->tasks.emplace([ex = *this, fn = std::forward<F>(f)]() mutable {
        detail::executor_guard g{any_executor{ex}};
        fn();
      });
    }

    state_->cv.notify_one();
  }

  template <class F>
    requires std::is_invocable_v<F&>
  void dispatch(F&& f) const noexcept {
    if (!state_) {
      return;
    }

    auto const cur_any = detail::get_current_executor();
    if (cur_any) {
      auto const* cur = detail::any_executor_access::target<basic_executor_type>(cur_any);
      if (cur != nullptr && (*cur == *this)) {
        f();
        return;
      }
    }

    post(std::forward<F>(f));
  }

  auto stopped() const noexcept -> bool {
    return !state_ || state_->stopped.load(std::memory_order_acquire);
  }

  explicit operator bool() const noexcept { return state_ != nullptr; }

  friend auto operator==(basic_executor_type const& a,
                         basic_executor_type const& b) noexcept -> bool {
    return a.state_.get() == b.state_.get();
  }
  friend auto operator!=(basic_executor_type const& a,
                         basic_executor_type const& b) noexcept -> bool {
    return !(a == b);
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

namespace iocoro::detail {

template <>
struct executor_traits<thread_pool::basic_executor_type> {
  static auto capabilities(thread_pool::basic_executor_type const& ex) noexcept
    -> executor_capability {
    return ex ? executor_capability::none : executor_capability::none;
  }

  static auto io_context(thread_pool::basic_executor_type const&) noexcept -> io_context_impl* {
    return nullptr;
  }
};

}  // namespace iocoro::detail

#include <iocoro/impl/thread_pool.ipp>

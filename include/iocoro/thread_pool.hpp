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
  class executor_type;

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
  enum class pool_state : std::uint8_t { running, draining, stopped };

  struct state {
    mutable std::mutex cv_mutex;
    std::condition_variable cv;
    std::deque<detail::unique_function<void()>> queue;
    std::atomic<std::size_t> pending{0};

    std::atomic<pool_state> lifecycle{pool_state::running};
    detail::work_guard_counter work_guard{};

    std::size_t n_threads{};

    std::atomic<std::shared_ptr<exception_handler_t>> on_task_exception{};

    auto can_accept_work() const noexcept -> bool {
      auto const lc = lifecycle.load(std::memory_order_acquire);
      if (lc == pool_state::running) {
        return true;
      }
      if (lc == pool_state::draining) {
        return work_guard.has_work();
      }
      return false;
    }

    auto request_stop() noexcept -> bool {
      auto expected = pool_state::running;
      return lifecycle.compare_exchange_strong(
        expected, pool_state::draining, std::memory_order_acq_rel);
    }
  };

  std::shared_ptr<state> state_;
  std::vector<std::thread> threads_;

  // Helper methods
  static void worker_loop(std::shared_ptr<state> s, std::size_t index);
};

/// A lightweight executor that schedules work onto a thread_pool.
///
/// Holds shared ownership of pool state for lifetime safety.
class thread_pool::executor_type {
 public:
  executor_type() noexcept = default;
  explicit executor_type(std::shared_ptr<state> s) noexcept : state_(std::move(s)) {}

  executor_type(executor_type const&) noexcept = default;
  auto operator=(executor_type const&) noexcept -> executor_type& = default;
  executor_type(executor_type&&) noexcept = default;
  auto operator=(executor_type&&) noexcept -> executor_type& = default;

  template <class F>
    requires std::is_invocable_v<F&>
  void post(F&& f) const noexcept {
    if (!state_) {
      return;
    }

    if (!state_->can_accept_work()) {
      return;
    }

    auto task = detail::unique_function<void()>{[ex = *this, fn = std::forward<F>(f)]() mutable {
      detail::executor_guard g{any_executor{ex}};
      fn();
    }};
    {
      std::scoped_lock lock{state_->cv_mutex};
      state_->queue.emplace_back(std::move(task));
      state_->pending.fetch_add(1, std::memory_order_release);
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
      auto const* cur = detail::any_executor_access::target<executor_type>(cur_any);
      if (cur != nullptr && (*cur == *this)) {
        f();
        return;
      }
    }

    post(std::forward<F>(f));
  }

  auto stopped() const noexcept -> bool {
    return !state_ ||
           state_->lifecycle.load(std::memory_order_acquire) != pool_state::running;
  }

  explicit operator bool() const noexcept { return state_ != nullptr; }

  friend auto operator==(executor_type const& a,
                         executor_type const& b) noexcept -> bool {
    return a.state_.get() == b.state_.get();
  }
  friend auto operator!=(executor_type const& a,
                         executor_type const& b) noexcept -> bool {
    return !(a == b);
  }

 private:
  friend class work_guard<executor_type>;

  void add_work_guard() const noexcept {
    if (state_) {
      state_->work_guard.add();
    }
  }

  void remove_work_guard() const noexcept {
    if (state_) {
      auto const old = state_->work_guard.remove();
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
struct executor_traits<thread_pool::executor_type> {
  static auto capabilities(thread_pool::executor_type const& ex) noexcept
    -> executor_capability {
    (void)ex;
    return executor_capability::none;
  }

  static auto io_context(thread_pool::executor_type const&) noexcept -> io_context_impl* {
    return nullptr;
  }
};

}  // namespace iocoro::detail

#include <iocoro/impl/thread_pool.ipp>

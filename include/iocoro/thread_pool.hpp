#pragma once

#include <iocoro/any_executor.hpp>
#include <iocoro/assert.hpp>
#include <iocoro/detail/executor_cast.hpp>
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

  /// Stop accepting new tasks and transition to draining mode (idempotent).
  ///
  /// Draining semantics:
  /// - New tasks posted after `stop()` are dropped.
  /// - Already-queued tasks may continue to run while workers are active.
  /// - Workers exit once queue is empty and work_guard count reaches zero.
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

  struct shared_state {
    mutable std::mutex cv_mutex{};
    std::condition_variable cv{};
    std::deque<detail::unique_function<void()>> queue{};
    std::size_t waiting_workers{0};
    state_t state{state_t::running};
    detail::work_guard_counter work_guard{};
    std::atomic<std::shared_ptr<exception_handler_t>> on_task_exception{};
  };

  std::shared_ptr<shared_state> state_{};
  std::size_t n_threads_{0};
  std::vector<std::thread> threads_;

  static void worker_loop(std::shared_ptr<shared_state> state, std::size_t index);
};

/// A lightweight executor that schedules work onto a thread_pool.
///
/// Holds shared ownership of pool state for lifetime safety.
class thread_pool::executor_type {
 public:
  executor_type() noexcept = default;
  explicit executor_type(std::shared_ptr<shared_state> state) noexcept : state_(std::move(state)) {}

  executor_type(executor_type const&) noexcept = default;
  auto operator=(executor_type const&) noexcept -> executor_type& = default;
  executor_type(executor_type&&) noexcept = default;
  auto operator=(executor_type&&) noexcept -> executor_type& = default;

  void post(detail::unique_function<void()> f) const noexcept {
    auto st = state_;
    if (!st) {
      return;
    }

    bool should_notify = false;
    {
      std::scoped_lock lock{st->cv_mutex};
      if (st->state != state_t::running) {
        return;
      }
      st->queue.emplace_back(std::move(f));
      should_notify = (st->waiting_workers > 0);
    }
    if (should_notify) {
      st->cv.notify_one();
    }
  }

  void dispatch(detail::unique_function<void()> f) const noexcept {
    auto st = state_;
    if (!st) {
      return;
    }

    auto const cur_any = detail::get_current_executor();
    if (cur_any) {
      auto const* cur = detail::any_executor_access::target<executor_type>(cur_any);
      if (cur != nullptr && cur->state_.get() == st.get()) {
        try {
          f();
        } catch (...) {
          auto handler_ptr = st->on_task_exception.load(std::memory_order_acquire);
          if (handler_ptr) {
            try {
              (*handler_ptr)(std::current_exception());
            } catch (...) {
              // Swallow exceptions from handler to preserve noexcept behavior.
            }
          }
        }
        return;
      }
    }

    post(std::move(f));
  }

  /// True if the pool has been stopped (or this executor is empty).
  auto stopped() const noexcept -> bool {
    auto st = state_;
    if (!st) {
      return true;
    }
    std::scoped_lock lock{st->cv_mutex};
    return st->state != state_t::running;
  }

  explicit operator bool() const noexcept { return state_ != nullptr; }

  friend auto operator==(executor_type const& a, executor_type const& b) noexcept -> bool {
    return a.state_.get() == b.state_.get();
  }
  friend auto operator!=(executor_type const& a, executor_type const& b) noexcept -> bool {
    return !(a == b);
  }

 private:
  friend class work_guard<executor_type>;

  void add_work_guard() const noexcept {
    auto st = state_;
    if (st) {
      st->work_guard.add();
    }
  }

  void remove_work_guard() const noexcept {
    auto st = state_;
    if (st) {
      auto const old = st->work_guard.remove();
      if (old == 1) {
        st->cv.notify_all();
      }
    }
  }

  std::shared_ptr<shared_state> state_{};
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

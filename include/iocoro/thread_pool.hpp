#pragma once

#include <iocoro/any_executor.hpp>
#include <iocoro/assert.hpp>
#include <iocoro/detail/executor_cast.hpp>
#include <iocoro/detail/executor_guard.hpp>
#include <iocoro/detail/lockfree_mpmc_queue.hpp>
#include <iocoro/detail/unique_function.hpp>
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

namespace detail {
struct thread_pool_worker_context {
  void const* current_state{};
  std::size_t worker_index{};
};

inline thread_local thread_pool_worker_context* thread_pool_ctx = nullptr;
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
  enum class pool_state : std::uint8_t { running, stopping, stopped };

  struct state {
    struct worker_state {
      std::mutex mutex;
      std::deque<detail::unique_function<void()>> local;
      std::atomic<bool> has_local{false};
    };

    static constexpr std::size_t global_capacity = 1u << 16;

    mutable std::mutex cv_mutex;
    std::condition_variable cv;
    detail::lockfree_mpmc_queue<detail::unique_function<void()>, global_capacity> global{};
    std::atomic<std::size_t> global_pending{0};

    std::vector<std::unique_ptr<worker_state>> workers;

    std::atomic<pool_state> lifecycle{pool_state::running};
    std::atomic<std::size_t> work_guard_count{0};

    std::size_t n_threads{};

    std::atomic<std::shared_ptr<exception_handler_t>> on_task_exception{};

    auto can_accept_work() const noexcept -> bool {
      auto const lc = lifecycle.load(std::memory_order_acquire);
      if (lc == pool_state::running) {
        return true;
      }
      return work_guard_count.load(std::memory_order_acquire) > 0;
    }

    auto request_stop() noexcept -> bool {
      auto expected = pool_state::running;
      return lifecycle.compare_exchange_strong(
        expected, pool_state::stopping, std::memory_order_acq_rel);
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

    auto weak = std::weak_ptr<state>{state_};
    auto make_task = [weak, fn = std::forward<F>(f)]() mutable {
      auto locked = weak.lock();
      if (!locked) {
        return;
      }
      detail::executor_guard g{any_executor{basic_executor_type{std::move(locked)}}};
      fn();
    };

    // If we're already running on this pool, defer until the current task
    // returns to the worker loop. This prevents destroying coroutine frames
    // while `final_suspend().await_suspend()` is still executing on-stack.
    auto* ctx = detail::thread_pool_ctx;
    if (ctx != nullptr && ctx->current_state == state_.get()) {
      auto& local = *state_->workers[ctx->worker_index];
      std::scoped_lock lock{local.mutex};
      local.local.emplace_back(std::move(make_task));
      local.has_local.store(true, std::memory_order_release);
      return;
    }

    if (!state_->can_accept_work()) {
      return;
    }

    auto task = detail::unique_function<void()>{std::move(make_task)};
    while (!state_->global.try_enqueue(task)) {
      if (!state_->can_accept_work()) {
        return;
      }
      std::this_thread::yield();
    }
    state_->global_pending.fetch_add(1, std::memory_order_acq_rel);

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
    return !state_ ||
           state_->lifecycle.load(std::memory_order_acquire) != pool_state::running;
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
    (void)ex;
    return executor_capability::none;
  }

  static auto io_context(thread_pool::basic_executor_type const&) noexcept -> io_context_impl* {
    return nullptr;
  }
};

}  // namespace iocoro::detail

#include <iocoro/impl/thread_pool.ipp>

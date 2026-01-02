#include <iocoro/thread_pool.hpp>

namespace iocoro {

// Thread-local storage for thread identification
thread_local std::size_t thread_pool::thread_id_ = 0;

inline auto thread_pool::get_executor() noexcept -> executor_type {
  return executor_type{*this};
}

inline auto thread_pool::running_in_pool_thread() const noexcept -> bool {
  auto tid = thread_id_;
  return tid >= 1 && tid <= n_threads_;
}

inline void thread_pool::worker_loop(std::size_t tid) {
  // Set thread-local ID for dispatch() checks
  thread_id_ = tid;

  // Set thread-local executor for coroutine integration
  detail::executor_guard ex_guard{any_executor{get_executor()}};

  while (true) {
    detail::unique_function<void()> task;

    {
      std::unique_lock lock{queue_mutex_};

      queue_cv_.wait(lock, [this] {
        bool has_tasks = !tasks_.empty();
        bool is_stopped = stopped_.load(std::memory_order_acquire);
        bool has_guards = work_guard_counter_.load(std::memory_order_acquire) > 0;

        // Wake up if: have tasks, or stopped, or (no tasks and no guards)
        return has_tasks || is_stopped || (!has_tasks && !has_guards);
      });

      // Check exit conditions
      bool is_stopped = stopped_.load(std::memory_order_acquire);
      bool has_tasks = !tasks_.empty();
      bool has_guards = work_guard_counter_.load(std::memory_order_acquire) > 0;

      if (is_stopped && (!has_tasks || !has_guards)) {
        // Stopped and either no tasks or no guards, exit
        return;
      }

      if (!has_tasks && !has_guards) {
        // No work and no guards, exit gracefully
        return;
      }

      if (has_tasks) {
        task = std::move(tasks_.front());
        tasks_.pop();
      }
    }

    // Execute task outside the lock
    if (task) {
      task();
    }
  }
}

inline thread_pool::thread_pool(std::size_t n_threads) : n_threads_(n_threads) {
  IOCORO_ENSURE(n_threads > 0, "thread_pool: n_threads must be > 0");

  threads_.reserve(n_threads);

  // Start worker threads
  for (std::size_t i = 0; i < n_threads; ++i) {
    threads_.emplace_back([this, tid = i + 1] {
      worker_loop(tid);
    });
  }
}

inline thread_pool::~thread_pool() {
  stop();
  join();
}

inline void thread_pool::stop() noexcept {
  stopped_.store(true, std::memory_order_release);
  queue_cv_.notify_all();
}

inline void thread_pool::join() noexcept {
  for (auto& t : threads_) {
    if (t.joinable()) {
      t.join();
    }
  }
}

}  // namespace iocoro

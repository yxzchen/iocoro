#include <iocoro/thread_pool.hpp>

namespace iocoro {

// Thread-local storage for thread identification - per-pool instance
thread_local thread_pool const* thread_pool::current_pool_ = nullptr;

inline auto thread_pool::get_executor() noexcept -> executor_type {
  return executor_type{*this};
}

inline auto thread_pool::running_in_pool_thread() const noexcept -> bool {
  return current_pool_ == this;
}

inline void thread_pool::worker_loop() {
  // Set thread-local pool pointer for dispatch() checks
  current_pool_ = this;

  while (true) {
    detail::unique_function<void()> task;

    {
      std::unique_lock lock{queue_mutex_};

      // Wait for: task available OR stopped
      queue_cv_.wait(lock, [this] {
        return !tasks_.empty() || stopped_.load(std::memory_order_acquire);
      });

      // Exit if stopped AND queue is drained
      if (stopped_.load(std::memory_order_acquire) && tasks_.empty()) {
        return;
      }

      // Get task if available
      if (!tasks_.empty()) {
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
    threads_.emplace_back([this] {
      worker_loop();
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

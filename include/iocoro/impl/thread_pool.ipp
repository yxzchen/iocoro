#include <iocoro/thread_pool.hpp>

namespace iocoro {

inline auto thread_pool::get_executor() noexcept -> executor_type {
  return executor_type{state_};
}

inline auto thread_pool::size() const noexcept -> std::size_t {
  return state_ ? state_->n_threads : 0;
}

inline void thread_pool::worker_loop() {
  // Set up executor context for coroutines
  detail::executor_guard ex_guard{any_executor{get_executor()}};

  while (true) {
    detail::unique_function<void()> task;

    {
      std::unique_lock lock{state_->mutex};

      // Wait for: task available OR stopped
      state_->cv.wait(lock, [this] {
        return !state_->tasks.empty() || state_->stopped.load(std::memory_order_acquire);
      });

      // Exit only if stopped AND queue is drained
      if (state_->stopped.load(std::memory_order_acquire) && state_->tasks.empty()) {
        return;
      }

      // Get task if available
      if (!state_->tasks.empty()) {
        task = std::move(state_->tasks.front());
        state_->tasks.pop();
      }
    }

    // Execute task outside the lock
    if (task) {
      task();
    }
  }
}

inline thread_pool::thread_pool(std::size_t n_threads) {
  IOCORO_ENSURE(n_threads > 0, "thread_pool: n_threads must be > 0");

  // Create shared state
  state_ = std::make_shared<state>();
  state_->n_threads = n_threads;

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
  if (state_) {
    state_->stopped.store(true, std::memory_order_release);
    state_->cv.notify_all();
  }
}

inline void thread_pool::join() noexcept {
  for (auto& t : threads_) {
    if (t.joinable()) {
      t.join();
    }
  }
}

}  // namespace iocoro

#include <iocoro/thread_pool.hpp>

#include <exception>

namespace iocoro {

inline auto thread_pool::get_executor() noexcept -> executor_type {
  return executor_type{this};
}

inline auto thread_pool::size() const noexcept -> std::size_t {
  return n_threads_;
}

inline void thread_pool::set_exception_handler(exception_handler_t handler) noexcept {
  if (handler) {
    on_task_exception_.store(std::make_shared<exception_handler_t>(std::move(handler)),
                             std::memory_order_release);
  } else {
    on_task_exception_.store(nullptr, std::memory_order_release);
  }
}

inline void thread_pool::worker_loop(thread_pool* pool, std::size_t /*index*/) {
  while (true) {
    detail::unique_function<void()> task;

    {
      std::unique_lock lock{pool->cv_mutex_};
      pool->cv_.wait(lock, [&] {
        return !pool->queue_.empty() ||
               (pool->state_ == state_t::draining && pool->work_guard_.count() == 0);
      });

      if (pool->queue_.empty()) {
        if (pool->state_ == state_t::draining && pool->work_guard_.count() == 0) {
          break;
        }
        continue;
      }

      task = std::move(pool->queue_.front());
      pool->queue_.pop_front();
    }

    try {
      task();
    } catch (...) {
      auto handler_ptr = pool->on_task_exception_.load(std::memory_order_acquire);
      if (handler_ptr) {
        try {
          (*handler_ptr)(std::current_exception());
        } catch (...) {
          // Swallow exceptions from handler to prevent thread termination
        }
      }
    }
  }
}

inline thread_pool::thread_pool(std::size_t n_threads) {
  IOCORO_ENSURE(n_threads > 0, "thread_pool: n_threads must be > 0");

  n_threads_ = n_threads;
  threads_.reserve(n_threads);

  for (std::size_t i = 0; i < n_threads; ++i) {
    threads_.emplace_back([this, i] { worker_loop(this, i); });
  }
}

inline thread_pool::~thread_pool() {
  stop();
  join();
}

inline void thread_pool::stop() noexcept {
  {
    std::scoped_lock lock{cv_mutex_};
    if (state_ == state_t::running) {
      state_ = state_t::draining;
    }
  }
  cv_.notify_all();
}

inline void thread_pool::join() noexcept {
  stop();
  for (auto& t : threads_) {
    if (t.joinable()) {
      t.join();
    }
  }
  {
    std::scoped_lock lock{cv_mutex_};
    state_ = state_t::stopped;
  }
}

}  // namespace iocoro

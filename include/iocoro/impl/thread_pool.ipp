#include <iocoro/thread_pool.hpp>

#include <exception>

namespace iocoro {

inline auto thread_pool::get_executor() noexcept -> executor_type { return executor_type{state_}; }

inline auto thread_pool::size() const noexcept -> std::size_t {
  return state_ ? state_->n_threads : 0;
}

inline void thread_pool::set_exception_handler(exception_handler_t handler) noexcept {
  if (!state_) {
    return;
  }
  if (handler) {
    state_->on_task_exception.store(
      std::make_shared<exception_handler_t>(std::move(handler)),
      std::memory_order_release);
  } else {
    state_->on_task_exception.store(nullptr, std::memory_order_release);
  }
}

inline void thread_pool::worker_loop(std::shared_ptr<state> s, std::size_t /*index*/) {
  while (true) {
    detail::unique_function<void()> task;

    {
      std::unique_lock lock{s->cv_mutex};
      s->cv.wait(lock, [&] {
        return s->pending.load(std::memory_order_acquire) > 0 ||
               (s->lifecycle.load(std::memory_order_acquire) == pool_state::draining &&
                s->work_guard.count() == 0);
      });

      if (s->pending.load(std::memory_order_acquire) == 0) {
        if (s->lifecycle.load(std::memory_order_acquire) == pool_state::draining &&
            s->work_guard.count() == 0) {
          break;
        }
        continue;
      }

      task = std::move(s->queue.front());
      s->queue.pop_front();
      s->pending.fetch_sub(1, std::memory_order_acq_rel);
    }

    try {
      task();
    } catch (...) {
      auto handler_ptr = s->on_task_exception.load(std::memory_order_acquire);
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

  // Create shared state
  state_ = std::make_shared<state>();
  state_->n_threads = n_threads;

  threads_.reserve(n_threads);

  // Start worker threads
  for (std::size_t i = 0; i < n_threads; ++i) {
    auto s = state_;
    threads_.emplace_back([s, i] { worker_loop(s, i); });
  }
}

inline thread_pool::~thread_pool() {
  stop();
  join();
}

inline void thread_pool::stop() noexcept {
  if (!state_) {
    return;
  }
  (void)state_->request_stop();
  state_->cv.notify_all();
}

inline void thread_pool::join() noexcept {
  stop();
  for (auto& t : threads_) {
    if (t.joinable()) {
      t.join();
    }
  }
  if (state_) {
    state_->lifecycle.store(pool_state::stopped, std::memory_order_release);
  }
}

}  // namespace iocoro

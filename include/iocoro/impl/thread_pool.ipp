#include <iocoro/thread_pool.hpp>

#include <exception>

namespace iocoro {

inline auto thread_pool::get_executor() noexcept -> executor_type { return executor_type{state_}; }

inline auto thread_pool::size() const noexcept -> std::size_t {
  return state_ ? state_->n_threads : 0;
}

inline void thread_pool::set_exception_handler(exception_handler_t handler) noexcept {
  std::scoped_lock lock{state_->mutex};
  state_->on_task_exception = std::move(handler);
}

inline void thread_pool::worker_loop(std::shared_ptr<state> s) {
  while (true) {
    detail::unique_function<void()> task;

    {
      std::unique_lock lock{s->mutex};

      // Wait for:
      // - task available, OR
      // - stop requested AND no work guards remain (so we can exit)
      s->cv.wait(lock, [&] {
        if (!s->tasks.empty()) {
          return true;
        }

        if (s->stopped.load(std::memory_order_acquire)) {
          return s->work_guard_count.load(std::memory_order_acquire) == 0;
        }

        return false;
      });

      auto const stopped = s->stopped.load(std::memory_order_acquire);
      auto const has_tasks = !s->tasks.empty();
      auto const has_guards = s->work_guard_count.load(std::memory_order_acquire) > 0;

      if (stopped && !has_tasks && !has_guards) {
        return;
      }

      // Get task if available
      if (has_tasks) {
        task = std::move(s->tasks.front());
        s->tasks.pop();
      }
    }

    // Execute task outside the lock
    auto run_task = [&](detail::unique_function<void()>& t) noexcept {
      try {
        t();
      } catch (...) {
        // Get exception handler under lock
        exception_handler_t handler;
        {
          std::scoped_lock lock{s->mutex};
          handler = s->on_task_exception;
        }

        // Call handler if set, otherwise swallow exception
        if (handler) {
          try {
            handler(std::current_exception());
          } catch (...) {
            // Swallow exceptions from handler to prevent thread termination
          }
        }
      }
    };

    if (task) {
      detail::thread_pool_tls.current_state = s.get();
      run_task(task);

      // Drain tasks posted from within this pool thread.
      // These are deferred so they can't run concurrently with the posting task.
      while (!detail::thread_pool_tls.deferred.empty()) {
        auto deferred_task = std::move(detail::thread_pool_tls.deferred.back());
        detail::thread_pool_tls.deferred.pop_back();
        run_task(deferred_task);
      }

      detail::thread_pool_tls.current_state = nullptr;
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
    threads_.emplace_back([s] { worker_loop(s); });
  }
}

inline thread_pool::~thread_pool() {
  stop();
  join();
}

inline void thread_pool::stop() noexcept {
  {
    std::scoped_lock lock{state_->mutex};
    state_->stopped.store(true, std::memory_order_release);
  }

  state_->cv.notify_all();
}

inline void thread_pool::join() noexcept {
  for (auto& t : threads_) {
    if (t.joinable()) {
      t.join();
    }
  }
}

}  // namespace iocoro

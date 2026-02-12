#include <iocoro/thread_pool.hpp>

#include <exception>

namespace iocoro {

inline auto thread_pool::get_executor() noexcept -> executor_type {
  return executor_type{state_};
}

inline auto thread_pool::size() const noexcept -> std::size_t {
  return n_threads_;
}

inline void thread_pool::set_exception_handler(exception_handler_t handler) noexcept {
  auto st = state_;
  if (!st) {
    return;
  }
  if (handler) {
    st->on_task_exception.store(std::make_shared<exception_handler_t>(std::move(handler)),
                                std::memory_order_release);
  } else {
    st->on_task_exception.store(nullptr, std::memory_order_release);
  }
}

inline void thread_pool::worker_loop(std::shared_ptr<shared_state> state, std::size_t /*index*/) {
  IOCORO_ENSURE(state != nullptr, "thread_pool::worker_loop: empty state");
  while (true) {
    detail::unique_function<void()> task;

    {
      std::unique_lock lock{state->cv_mutex};
      state->cv.wait(lock, [&] {
        return !state->queue.empty() ||
               (state->state == state_t::draining && state->work_guard.count() == 0);
      });

      if (state->queue.empty()) {
        IOCORO_ASSERT(state->state == state_t::draining && state->work_guard.count() == 0);
        break;
      }

      task = std::move(state->queue.front());
      state->queue.pop_front();
    }

    try {
      task();
    } catch (...) {
      auto handler_ptr = state->on_task_exception.load(std::memory_order_acquire);
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

inline thread_pool::thread_pool(std::size_t n_threads)
    : state_(std::make_shared<shared_state>()), n_threads_{n_threads} {
  IOCORO_ENSURE(n_threads > 0, "thread_pool: n_threads must be > 0");

  threads_.reserve(n_threads_);

  for (std::size_t i = 0; i < n_threads_; ++i) {
    threads_.emplace_back([state = state_, i] { worker_loop(std::move(state), i); });
  }
}

inline thread_pool::~thread_pool() noexcept {
  join();
}

inline void thread_pool::stop() noexcept {
  auto st = state_;
  if (!st) {
    return;
  }
  {
    std::scoped_lock lock{st->cv_mutex};
    if (st->state == state_t::running) {
      st->state = state_t::draining;
    }
  }
  st->cv.notify_all();
}

inline void thread_pool::join() noexcept {
  stop();
  for (auto& t : threads_) {
    if (t.joinable()) {
      t.join();
    }
  }
  auto st = state_;
  if (!st) {
    return;
  }
  {
    std::scoped_lock lock{st->cv_mutex};
    st->state = state_t::stopped;
  }
}

}  // namespace iocoro

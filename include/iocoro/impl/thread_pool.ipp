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

inline void thread_pool::worker_loop(std::shared_ptr<state> s, std::size_t index) {
  detail::thread_pool_worker_context ctx{};
  ctx.current_state = s.get();
  ctx.worker_index = index;
  detail::thread_pool_ctx = &ctx;

  auto& local = *s->workers[index];

  while (true) {
    detail::unique_function<void()> task;

    auto pop_local = [&]() noexcept -> bool {
      if (!local.has_local.load(std::memory_order_acquire)) {
        return false;
      }
      std::scoped_lock lock{local.mutex};
      if (local.local.empty()) {
        local.has_local.store(false, std::memory_order_release);
        return false;
      }
      task = std::move(local.local.front());
      local.local.pop_front();
      if (local.local.empty()) {
        local.has_local.store(false, std::memory_order_release);
      }
      return true;
    };

    auto pop_global = [&]() noexcept -> bool {
      if (s->global_pending.load(std::memory_order_acquire) == 0) {
        return false;
      }
      if (!s->global.try_dequeue(task)) {
        return false;
      }
      s->global_pending.fetch_sub(1, std::memory_order_acq_rel);
      return true;
    };

    auto steal_from_others = [&]() noexcept -> bool {
      for (std::size_t i = 0; i < s->workers.size(); ++i) {
        if (i == index) {
          continue;
        }
        auto& other = *s->workers[i];
        if (!other.has_local.load(std::memory_order_acquire)) {
          continue;
        }
        std::scoped_lock lock{other.mutex};
        if (other.local.empty()) {
          other.has_local.store(false, std::memory_order_release);
          continue;
        }
        task = std::move(other.local.back());
        other.local.pop_back();
        if (other.local.empty()) {
          other.has_local.store(false, std::memory_order_release);
        }
        return true;
      }
      return false;
    };

    auto local_empty = [&]() noexcept -> bool {
      std::scoped_lock lock{local.mutex};
      return local.local.empty();
    };

    if (!pop_local() && !pop_global() && !steal_from_others()) {
      std::unique_lock lock{s->cv_mutex};
      s->cv.wait(lock, [&] {
        if (s->global_pending.load(std::memory_order_acquire) > 0) {
          return true;
        }
        if (s->lifecycle.load(std::memory_order_acquire) == pool_state::stopping &&
            s->work_guard_count.load(std::memory_order_acquire) == 0) {
          return true;
        }
        return false;
      });

      if (s->lifecycle.load(std::memory_order_acquire) == pool_state::stopping &&
          s->work_guard_count.load(std::memory_order_acquire) == 0 &&
          s->global_pending.load(std::memory_order_acquire) == 0 && local_empty()) {
        break;
      }
      continue;
    }

    // Execute task outside the lock
    auto run_task = [&](detail::unique_function<void()>& t) noexcept {
      try {
        t();
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
    };

    if (task) {
      run_task(task);
    }
  }

  detail::thread_pool_ctx = nullptr;
}

inline thread_pool::thread_pool(std::size_t n_threads) {
  IOCORO_ENSURE(n_threads > 0, "thread_pool: n_threads must be > 0");

  // Create shared state
  state_ = std::make_shared<state>();
  state_->n_threads = n_threads;
  state_->workers.reserve(n_threads);
  for (std::size_t i = 0; i < n_threads; ++i) {
    state_->workers.emplace_back(std::make_unique<state::worker_state>());
  }

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
  auto expected = pool_state::running;
  if (state_->lifecycle.compare_exchange_strong(
        expected, pool_state::stopping, std::memory_order_acq_rel)) {
    state_->cv.notify_all();
  } else {
    state_->cv.notify_all();
  }
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

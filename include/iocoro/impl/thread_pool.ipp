#include <iocoro/thread_pool.hpp>

#include <iocoro/thread_pool_executor.hpp>

namespace iocoro {

inline thread_pool::thread_pool(std::size_t n_threads) {
  IOCORO_ENSURE(n_threads > 0, "thread_pool: n_threads must be > 0");

  contexts_.reserve(n_threads);
  guards_.reserve(n_threads);
  threads_.reserve(n_threads);

  for (std::size_t i = 0; i < n_threads; ++i) {
    contexts_.push_back(std::make_unique<io_context>());
  }

  // Keep each shard alive until the pool is stopped/joined.
  for (auto& ctx : contexts_) {
    guards_.push_back(make_work_guard(*ctx));
  }

  // Start one thread per shard. Each thread sets the shard's thread id internally.
  for (std::size_t i = 0; i < contexts_.size(); ++i) {
    threads_.emplace_back([this, i] {
      (void)contexts_[i]->run();
    });
  }
}

inline thread_pool::~thread_pool() {
  stop();
  join();
}

inline void thread_pool::stop() noexcept {
  for (auto& ctx : contexts_) {
    if (ctx) {
      ctx->stop();
    }
  }
}

inline void thread_pool::join() noexcept {
  for (auto& t : threads_) {
    if (t.joinable()) {
      t.join();
    }
  }
}

inline auto thread_pool::pick_executor() noexcept -> executor {
  IOCORO_ENSURE(!contexts_.empty(), "thread_pool: no shards");
  auto const i = rr_.fetch_add(1, std::memory_order_relaxed);
  return contexts_[i % contexts_.size()]->get_executor();
}

inline auto thread_pool::get_executor() noexcept -> thread_pool_executor {
  return thread_pool_executor{*this};
}

}  // namespace iocoro



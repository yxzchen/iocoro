#pragma once

#include <iocoro/assert.hpp>
#include <iocoro/executor.hpp>
#include <iocoro/io_context.hpp>
#include <iocoro/work_guard.hpp>

#include <atomic>
#include <cstddef>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

namespace iocoro {

class thread_pool_executor;

/// A simple thread pool that runs multiple io_context instances (shards).
///
/// Design:
/// - Owns N independent io_context shards.
/// - Starts N worker threads, each running one shard's event loop.
/// - Provides round-robin executor selection via pick_executor().
///
/// Notes:
/// - This is intentionally minimal and does not attempt to provide advanced scheduling
///   policies. It is primarily a building block for higher-level executors.
class thread_pool {
 public:
  explicit thread_pool(std::size_t n_threads) {
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

  thread_pool(thread_pool const&) = delete;
  auto operator=(thread_pool const&) -> thread_pool& = delete;
  thread_pool(thread_pool&&) = delete;
  auto operator=(thread_pool&&) -> thread_pool& = delete;

  ~thread_pool() {
    stop();
    join();
  }

  auto get_executor() noexcept -> thread_pool_executor;

  /// Stop all shards (best-effort, idempotent).
  void stop() noexcept {
    for (auto& ctx : contexts_) {
      if (ctx) {
        ctx->stop();
      }
    }
  }

  /// Join all worker threads (best-effort, idempotent).
  void join() noexcept {
    for (auto& t : threads_) {
      if (t.joinable()) {
        t.join();
      }
    }
  }

  /// Select a shard executor (round-robin).
  auto pick_executor() noexcept -> executor {
    IOCORO_ENSURE(!contexts_.empty(), "thread_pool: no shards");
    auto const i = rr_.fetch_add(1, std::memory_order_relaxed);
    return contexts_[i % contexts_.size()]->get_executor();
  }

  auto size() const noexcept -> std::size_t { return contexts_.size(); }

 private:
  std::vector<std::unique_ptr<io_context>> contexts_{};
  std::vector<work_guard<executor>> guards_{};
  std::vector<std::thread> threads_{};

  std::atomic<std::size_t> rr_{0};
};

}  // namespace iocoro



#pragma once

#include <iocoro/assert.hpp>
#include <iocoro/io_executor.hpp>
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
/// - Provides round-robin io_executor selection via pick_executor().
///
/// Notes:
/// - This is intentionally minimal and does not attempt to provide advanced scheduling
///   policies. It is primarily a building block for higher-level executors.
class thread_pool {
 public:
  explicit thread_pool(std::size_t n_threads);

  thread_pool(thread_pool const&) = delete;
  auto operator=(thread_pool const&) -> thread_pool& = delete;
  thread_pool(thread_pool&&) = delete;
  auto operator=(thread_pool&&) -> thread_pool& = delete;

  ~thread_pool();

  auto get_executor() noexcept -> thread_pool_executor;

  /// Stop all shards (best-effort, idempotent).
  void stop() noexcept;

  /// Join all worker threads (best-effort, idempotent).
  void join() noexcept;

  /// Select a shard io_executor (round-robin).
  auto pick_executor() noexcept -> io_executor;

  auto size() const noexcept -> std::size_t { return contexts_.size(); }

 private:
  std::vector<std::unique_ptr<io_context>> contexts_{};
  std::vector<work_guard<io_executor>> guards_{};
  std::vector<std::thread> threads_{};

  std::atomic<std::size_t> rr_{0};
};

}  // namespace iocoro

#include <iocoro/impl/thread_pool.ipp>



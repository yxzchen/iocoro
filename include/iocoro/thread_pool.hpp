#pragma once

#include <iocoro/assert.hpp>
#include <iocoro/io_context.hpp>
#include <iocoro/io_executor.hpp>
#include <iocoro/work_guard.hpp>

#include <atomic>
#include <cstddef>
#include <memory>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace iocoro {

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
  class basic_executor_type;
  typedef basic_executor_type executor_type;

  explicit thread_pool(std::size_t n_threads);

  thread_pool(thread_pool const&) = delete;
  auto operator=(thread_pool const&) -> thread_pool& = delete;
  thread_pool(thread_pool&&) = delete;
  auto operator=(thread_pool&&) -> thread_pool& = delete;

  ~thread_pool();

  auto get_executor() noexcept -> executor_type;

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

/// A lightweight executor that schedules work onto a thread_pool.
///
/// This is a non-owning handle: the referenced thread_pool must outlive this object.
class thread_pool::basic_executor_type {
 public:
  basic_executor_type() noexcept = default;
  explicit basic_executor_type(thread_pool& pool) noexcept : pool_(&pool) {}

  basic_executor_type(basic_executor_type const&) noexcept = default;
  auto operator=(basic_executor_type const&) noexcept -> basic_executor_type& = default;
  basic_executor_type(basic_executor_type&&) noexcept = default;
  auto operator=(basic_executor_type&&) noexcept -> basic_executor_type& = default;

  template <class F>
    requires std::is_invocable_v<F&>
  void post(F&& f) const noexcept {
    IOCORO_ENSURE(pool_ != nullptr, "thread_pool::executor: empty pool_");
    pool_->pick_executor().post(std::forward<F>(f));
  }

  template <class F>
    requires std::is_invocable_v<F&>
  void dispatch(F&& f) const noexcept {
    IOCORO_ENSURE(pool_ != nullptr, "thread_pool::executor: empty pool_");
    pool_->pick_executor().dispatch(std::forward<F>(f));
  }

  auto pick_executor() const -> io_executor {
    IOCORO_ENSURE(pool_ != nullptr, "thread_pool::executor: empty pool_");
    return pool_->pick_executor();
  }

  auto stopped() const noexcept -> bool { return pool_ == nullptr; }

  explicit operator bool() const noexcept { return pool_ != nullptr; }

 private:
  thread_pool* pool_ = nullptr;
};

}  // namespace iocoro

#include <iocoro/impl/thread_pool.ipp>

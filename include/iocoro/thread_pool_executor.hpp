#pragma once

#include <iocoro/assert.hpp>
#include <iocoro/executor.hpp>
#include <iocoro/thread_pool.hpp>

#include <functional>
#include <utility>

namespace iocoro {

/// A lightweight executor that schedules work onto a thread_pool.
///
/// This is a non-owning handle: the referenced thread_pool must outlive this object.
class thread_pool_executor {
 public:
  thread_pool_executor() noexcept = default;
  explicit thread_pool_executor(thread_pool& pool) noexcept : pool_(&pool) {}

  thread_pool_executor(thread_pool_executor const&) noexcept = default;
  auto operator=(thread_pool_executor const&) noexcept -> thread_pool_executor& = default;
  thread_pool_executor(thread_pool_executor&&) noexcept = default;
  auto operator=(thread_pool_executor&&) noexcept -> thread_pool_executor& = default;

  void post(std::function<void()> f) const {
    IOCORO_ENSURE(pool_ != nullptr, "thread_pool_executor: empty pool_");
    pool_->pick_executor().post(std::move(f));
  }

  void dispatch(std::function<void()> f) const {
    IOCORO_ENSURE(pool_ != nullptr, "thread_pool_executor: empty pool_");
    pool_->pick_executor().dispatch(std::move(f));
  }

  auto stopped() const noexcept -> bool { return pool_ == nullptr; }

  explicit operator bool() const noexcept { return pool_ != nullptr; }

 private:
  thread_pool* pool_ = nullptr;
};

inline auto thread_pool::get_executor() noexcept -> thread_pool_executor {
  return thread_pool_executor{*this};
}

}  // namespace iocoro

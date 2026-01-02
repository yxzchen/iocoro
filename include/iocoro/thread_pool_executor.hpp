#pragma once

#include <iocoro/assert.hpp>
#include <iocoro/detail/unique_function.hpp>
#include <iocoro/io_executor.hpp>
#include <iocoro/thread_pool.hpp>

#include <type_traits>
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

  template <class F>
    requires std::is_invocable_v<F&>
  void post(F&& f) const noexcept {
    IOCORO_ENSURE(pool_ != nullptr, "thread_pool_executor: empty pool_");
    pool_->pick_executor().post(std::forward<F>(f));
  }

  template <class F>
    requires std::is_invocable_v<F&>
  void dispatch(F&& f) const noexcept {
    IOCORO_ENSURE(pool_ != nullptr, "thread_pool_executor: empty pool_");
    pool_->pick_executor().dispatch(std::forward<F>(f));
  }

  auto pick_executor() const -> io_executor {
    IOCORO_ENSURE(pool_ != nullptr, "thread_pool_executor: empty pool_");
    return pool_->pick_executor();
  }

  auto stopped() const noexcept -> bool { return pool_ == nullptr; }

  explicit operator bool() const noexcept { return pool_ != nullptr; }

 private:
  thread_pool* pool_ = nullptr;
};

}  // namespace iocoro

#include <iocoro/impl/thread_pool_executor.ipp>

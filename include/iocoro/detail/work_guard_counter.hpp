#pragma once

#include <iocoro/assert.hpp>

#include <atomic>
#include <cstddef>

namespace iocoro::detail {

class work_guard_counter {
 public:
  void add() noexcept { count_.fetch_add(1, std::memory_order_acq_rel); }

  auto remove() noexcept -> std::size_t {
    auto const old = count_.fetch_sub(1, std::memory_order_acq_rel);
    IOCORO_ENSURE(old > 0, "work_guard_counter: remove() without add()");
    return old;
  }

  auto count() const noexcept -> std::size_t { return count_.load(std::memory_order_acquire); }

  auto has_work() const noexcept -> bool { return count() > 0; }

 private:
  std::atomic<std::size_t> count_{0};
};

}  // namespace iocoro::detail

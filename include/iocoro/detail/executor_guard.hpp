#pragma once

#include <iocoro/any_executor.hpp>
#include <iocoro/assert.hpp>

#include <coroutine>

namespace iocoro::detail {

inline thread_local any_executor current_executor{};

inline auto get_current_executor() noexcept -> any_executor {
  return current_executor;
}

struct executor_guard {
  any_executor prev;

  explicit executor_guard(any_executor ex) noexcept : prev(current_executor) {
    current_executor = std::move(ex);
  }

  template <executor Ex>
  explicit executor_guard(Ex ex) noexcept : executor_guard(any_executor{std::move(ex)}) {}

  ~executor_guard() { current_executor = prev; }

  executor_guard(executor_guard const&) = delete;
  auto operator=(executor_guard const&) -> executor_guard& = delete;
  executor_guard(executor_guard&&) = delete;
  auto operator=(executor_guard&&) -> executor_guard& = delete;
};

}  // namespace iocoro::detail

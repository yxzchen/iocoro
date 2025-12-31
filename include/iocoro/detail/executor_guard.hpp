#pragma once

#include <iocoro/executor.hpp>

#include <coroutine>

namespace iocoro::detail {

inline thread_local any_executor current_executor{};

inline auto get_current_executor() noexcept -> any_executor { return current_executor; }

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

/// Helper to resume a coroutine on a specific executor with proper guard.
/// IMPORTANT: Copies executor to avoid leaving source empty.
inline void resume_on_executor(any_executor ex, std::coroutine_handle<> h) {
  ex.post([h, ex]() mutable { h.resume(); });
}

}  // namespace iocoro::detail

#pragma once

#include <xz/io/executor.hpp>

namespace xz::io::detail {

inline thread_local executor current_executor{};

inline auto get_current_executor() noexcept -> executor { return current_executor; }

struct executor_guard {
  executor prev;

  explicit executor_guard(executor ex) noexcept : prev(current_executor) {
    current_executor = ex;
  }
  ~executor_guard() { current_executor = prev; }

  executor_guard(executor_guard const&) = delete;
  auto operator=(executor_guard const&) -> executor_guard& = delete;
  executor_guard(executor_guard&&) = delete;
  auto operator=(executor_guard&&) -> executor_guard& = delete;
};

}  // namespace xz::io::detail

#pragma once

#include <iocoro/io_executor.hpp>

namespace iocoro::detail {

inline thread_local io_executor current_executor{};

inline auto get_current_executor() noexcept -> io_executor { return current_executor; }

struct executor_guard {
  io_executor prev;

  explicit executor_guard(io_executor ex) noexcept : prev(current_executor) { current_executor = ex; }
  ~executor_guard() { current_executor = prev; }

  executor_guard(executor_guard const&) = delete;
  auto operator=(executor_guard const&) -> executor_guard& = delete;
  executor_guard(executor_guard&&) = delete;
  auto operator=(executor_guard&&) -> executor_guard& = delete;
};

}  // namespace iocoro::detail

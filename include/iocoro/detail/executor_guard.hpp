#pragma once

#include <iocoro/executor.hpp>
#include <iocoro/io_executor.hpp>

namespace iocoro::detail {

inline thread_local any_executor current_executor{};
inline thread_local io_executor current_io_executor{};

inline auto get_current_executor() noexcept -> any_executor { return current_executor; }
inline auto get_current_io_executor() noexcept -> io_executor { return current_io_executor; }

struct executor_guard {
  any_executor prev;
  io_executor prev_io;

  explicit executor_guard(any_executor ex) noexcept
    : prev(current_executor), prev_io(current_io_executor) {
    current_executor = std::move(ex);
    current_io_executor = {};
  }

  explicit executor_guard(io_executor ex) noexcept : prev(current_executor), prev_io(current_io_executor) {
    current_executor = any_executor{ex};
    current_io_executor = ex;
  }

  template <executor Ex>
  explicit executor_guard(Ex ex) noexcept : executor_guard(any_executor{std::move(ex)}) {}

  ~executor_guard() {
    current_executor = prev;
    current_io_executor = prev_io;
  }

  executor_guard(executor_guard const&) = delete;
  auto operator=(executor_guard const&) -> executor_guard& = delete;
  executor_guard(executor_guard&&) = delete;
  auto operator=(executor_guard&&) -> executor_guard& = delete;
};

}  // namespace iocoro::detail

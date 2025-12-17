#pragma once

#include <xz/io/io_context.hpp>
#include <xz/io/detail/current_executor.hpp>

#include <coroutine>

namespace xz::io::this_coro {

struct executor_t {
  explicit executor_t() = default;
};

inline constexpr executor_t executor{};

}  // namespace xz::io::this_coro

namespace xz::io::detail {

struct this_executor_awaiter {
  auto await_ready() const noexcept -> bool { return true; }
  void await_suspend(std::coroutine_handle<>) const noexcept {}
  auto await_resume() const noexcept -> io_context& {
    // Precondition: running inside io_context.
    return get_current_executor();
  }
};

}  // namespace xz::io::detail



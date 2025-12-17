#pragma once

#include <xz/io/io_context.hpp>

#include <coroutine>
#include <utility>

namespace xz::io::detail {

// Single-threaded event loop: track the io_context currently executing callbacks/resumptions.
inline thread_local io_context* current_executor = nullptr;

struct executor_guard {
  io_context* prev_ = nullptr;

  explicit executor_guard(io_context& ex) noexcept : prev_(std::exchange(current_executor, &ex)) {}

  executor_guard(executor_guard const&) = delete;
  auto operator=(executor_guard const&) -> executor_guard& = delete;

  ~executor_guard() { current_executor = prev_; }
};

inline auto try_get_current_executor() noexcept -> io_context* { return current_executor; }

inline auto get_current_executor() -> io_context& { return *current_executor; }

inline void defer_resume(std::coroutine_handle<> h) {
  if (!h) return;
  if (auto* ex = try_get_current_executor()) {
    ex->post([h]() mutable { h.resume(); });
  } else {
    // Fallback: outside io_context execution we can't post; resume inline.
    h.resume();
  }
}

inline void defer_start(std::coroutine_handle<> h) {
  // Same semantics as defer_resume; separate name for clarity.
  defer_resume(h);
}

}  // namespace xz::io::detail



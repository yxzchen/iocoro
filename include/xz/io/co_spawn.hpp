#pragma once

#include <xz/io/awaitable.hpp>
#include <xz/io/executor.hpp>

namespace xz::io {

/// Start an awaitable on the given executor (cold coroutine model).
///
/// Ownership of the coroutine frame is detached from the passed awaitable:
/// the frame will be destroyed at final_suspend.
template <typename T>
void co_spawn(executor ex, awaitable<T> a) {
  auto h = a.coro_;
  a.coro_ = {};

  h.promise().set_executor(ex);
  h.promise().detach();

  ex.post([h, ex]() mutable {
    detail::executor_guard g{ex};
    try {
      h.resume();
    } catch (...) {
      // Detached mode: swallow exceptions
    }
  });
}

}  // namespace xz::io

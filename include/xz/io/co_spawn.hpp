#pragma once

#include <xz/io/detached.hpp>
#include <xz/io/detail/spawn.hpp>
#include <xz/io/executor.hpp>
#include <xz/io/use_awaitable.hpp>

namespace xz::io {

/// Start an awaitable on the given executor (detached / fire-and-forget).
///
/// Ownership of the coroutine frame is detached from the passed awaitable:
/// the frame will be destroyed at final_suspend.
template <typename T>
void co_spawn(executor ex, awaitable<T> a, detached_t) {
  auto h = a.release();

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

/// Start an awaitable on the given executor, returning an awaitable that can be awaited
/// to obtain the result (exception is rethrown on await_resume()).
template <typename T>
auto co_spawn(executor ex, awaitable<T> a, use_awaitable_t) -> awaitable<T> {
  // Hot-start: start running immediately (via detached runner), and return an awaitable
  // that only waits for completion and yields the result.
  auto st = std::make_shared<detail::spawn_state<T>>(ex);

  co_spawn(ex, detail::run_to_state<T>(ex, st, std::move(a)), detached);
  return detail::await_state<T>(std::move(st));
}

/// Start an awaitable on the given executor, invoking a completion callback with either
/// the result or an exception.
template <typename T, typename F>
  requires detail::completion_callback_for<F, T>
void co_spawn(executor ex, awaitable<T> a, F&& completion) {
  using completion_t = std::remove_cvref_t<F>;
  co_spawn(ex,
           detail::completion_wrapper<T, completion_t>(ex, std::move(a),
                                                       completion_t(std::forward<F>(completion))),
           detached);
}

}  // namespace xz::io

#pragma once

#include <iocoro/detached.hpp>
#include <iocoro/detail/spawn.hpp>
#include <iocoro/executor.hpp>
#include <iocoro/use_awaitable.hpp>

namespace iocoro {

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

/// Start a callable that returns iocoro::awaitable<T> on the given executor (detached).
///
/// This overload is the safe/idiomatic way to spawn coroutine lambdas with captures:
/// passing `[](...) -> awaitable<T> { ... }()` directly can leave the coroutine holding
/// a dangling `this` pointer to a temporary closure object (GCC/ASan).
template <typename F>
  requires detail::awaitable_factory<std::remove_cvref_t<F>>
void co_spawn(executor ex, F&& f, detached_t) {
  co_spawn(ex, detail::invoke_and_await(std::remove_cvref_t<F>(std::forward<F>(f))), detached);
}

/// Start a callable that returns iocoro::awaitable<T> on the given executor, returning an
/// awaitable that can be awaited to obtain the result.
template <typename F>
  requires detail::awaitable_factory<std::remove_cvref_t<F>>
auto co_spawn(executor ex, F&& f, use_awaitable_t)
  -> awaitable<detail::awaitable_value_t<std::remove_cvref_t<F>>> {
  return co_spawn(ex, detail::invoke_and_await(std::remove_cvref_t<F>(std::forward<F>(f))),
                  use_awaitable);
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

/// Start a callable that returns iocoro::awaitable<T> on the given executor, invoking a
/// completion callback with either the result or an exception.
template <typename Factory, typename Completion>
  requires detail::awaitable_factory<std::remove_cvref_t<Factory>> &&
           detail::completion_callback_for<std::remove_cvref_t<Completion>,
                                           detail::awaitable_value_t<std::remove_cvref_t<Factory>>>
void co_spawn(executor ex, Factory&& f, Completion&& completion) {
  co_spawn(ex, detail::invoke_and_await(std::remove_cvref_t<Factory>(std::forward<Factory>(f))),
           std::forward<Completion>(completion));
}

}  // namespace iocoro

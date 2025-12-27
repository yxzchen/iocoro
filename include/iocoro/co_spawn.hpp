#pragma once

#include <iocoro/completion_token.hpp>
#include <iocoro/detail/spawn.hpp>
#include <iocoro/executor.hpp>

namespace iocoro {

/// Start an awaitable on the given executor (detached / fire-and-forget).
///
/// Ownership of the coroutine frame is detached from the passed awaitable:
/// the frame will be destroyed at final_suspend.
template <typename T>
void co_spawn(executor ex, awaitable<T> a, detached_t) {
  detail::awaitable_as_function<T> wrapper(std::move(a));
  using wrapper_type = detail::awaitable_as_function<T>;

  auto state = detail::spawn_state<executor, wrapper_type>(ex, std::move(wrapper));
  auto entry = detail::spawn_entry_point<T>(std::move(state));

  detail::spawn_detached_impl(ex, std::move(entry));
}

/// Start a callable that returns iocoro::awaitable<T> on the given executor (detached).
///
/// This overload is the safe/idiomatic way to spawn coroutine lambdas with captures:
/// passing `[](...) -> awaitable<T> { ... }()` directly can leave the coroutine holding
/// a dangling `this` pointer to a temporary closure object (GCC/ASan).
template <typename F>
  requires detail::awaitable_factory<std::remove_cvref_t<F>>
void co_spawn(executor ex, F&& f, detached_t) {
  using function_type = std::decay_t<F>;
  using value_type = detail::awaitable_value_t<function_type>;

  auto state = detail::spawn_state<executor, function_type>(ex, std::forward<F>(f));
  auto entry = detail::spawn_entry_point<value_type>(std::move(state));

  detail::spawn_detached_impl(ex, std::move(entry));
}

/// Start an awaitable on the given executor, returning an awaitable that can be awaited
/// to obtain the result (exception is rethrown on await_resume()).
template <typename T>
auto co_spawn(executor ex, awaitable<T> a, use_awaitable_t) -> awaitable<T> {
  // Hot-start: start running immediately (via detached runner), and return an awaitable
  // that only waits for completion and yields the result.
  auto st = std::make_shared<detail::spawn_wait_state<T>>(ex);

  co_spawn(ex, detail::run_to_state<T>(ex, st, std::move(a)), detached);
  return detail::await_state<T>(std::move(st));
}

/// Start a callable that returns iocoro::awaitable<T> on the given executor, returning an
/// awaitable that can be awaited to obtain the result.
template <typename F>
  requires detail::awaitable_factory<std::remove_cvref_t<F>>
auto co_spawn(executor ex, F&& f, use_awaitable_t)
  -> awaitable<detail::awaitable_value_t<std::remove_cvref_t<F>>> {
  using function_type = std::decay_t<F>;
  using value_type = detail::awaitable_value_t<function_type>;

  // Preserve hot-start semantics (tests rely on it): start immediately (once the context runs),
  // and return an awaitable that only waits for completion and yields the result.
  auto st = std::make_shared<detail::spawn_wait_state<value_type>>(ex);

  auto state = detail::spawn_state<executor, function_type>(ex, std::forward<F>(f));
  auto entry = detail::spawn_entry_point<value_type>(std::move(state));

  co_spawn(ex, detail::run_to_state<value_type>(ex, st, std::move(entry)), detached);
  return detail::await_state<value_type>(std::move(st));
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
  using function_type = std::decay_t<Factory>;
  using value_type = detail::awaitable_value_t<function_type>;

  auto state = detail::spawn_state<executor, function_type>(ex, std::forward<Factory>(f));
  auto entry = detail::spawn_entry_point<value_type>(std::move(state));

  co_spawn(ex, std::move(entry), std::forward<Completion>(completion));
}

}  // namespace iocoro

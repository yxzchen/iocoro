#pragma once

#include <iocoro/completion_token.hpp>
#include <iocoro/detail/spawn.hpp>
#include <iocoro/executor.hpp>
#include <iocoro/thread_pool_executor.hpp>

namespace iocoro {

/// Start an awaitable on the given executor (detached / fire-and-forget).
///
/// Ownership of the coroutine frame is detached from the passed awaitable:
/// the frame will be destroyed at final_suspend.
template <typename T>
void co_spawn(executor ex, awaitable<T> a, detached_t) {
  detail::awaitable_as_function<T> wrapper(std::move(a));

  auto state = std::make_shared<detail::spawn_state<T>>(std::move(wrapper));
  auto entry = detail::spawn_entry_point<T>(std::move(state));

  detail::spawn_detached_impl(ex, std::move(entry));
}

/// Start an awaitable on the given thread_pool_executor (detached / fire-and-forget).
template <typename T>
void co_spawn(thread_pool_executor pex, awaitable<T> a, detached_t) {
  auto ex = pex.pick_executor();
  co_spawn(ex, std::move(a), detached);
}

/// Start a callable that returns iocoro::awaitable<T> on the given executor (detached).
///
/// This overload is the safe/idiomatic way to spawn coroutine lambdas with captures:
/// passing `[](...) -> awaitable<T> { ... }()` directly can leave the coroutine holding
/// a dangling `this` pointer to a temporary closure object (GCC/ASan).
template <typename F>
  requires detail::awaitable_factory<std::remove_cvref_t<F>>
void co_spawn(executor ex, F&& f, detached_t) {
  using value_type = detail::awaitable_value_t<std::remove_cvref_t<F>>;

  auto state = std::make_shared<detail::spawn_state<value_type>>(std::forward<F>(f));
  auto entry = detail::spawn_entry_point<value_type>(std::move(state));

  detail::spawn_detached_impl(ex, std::move(entry));
}

/// Start a callable that returns iocoro::awaitable<T> on the given thread_pool_executor (detached).
template <typename F>
  requires detail::awaitable_factory<std::remove_cvref_t<F>>
void co_spawn(thread_pool_executor pex, F&& f, detached_t) {
  auto ex = pex.pick_executor();
  co_spawn(ex, std::forward<F>(f), detached);
}

/// Start an awaitable on the given executor, returning an awaitable that can be awaited
/// to obtain the result (exception is rethrown on await_resume()).
template <typename T>
auto co_spawn(executor ex, awaitable<T> a, use_awaitable_t) -> awaitable<T> {
  // Hot-start: start running immediately (via detached runner), and return an awaitable
  // that only waits for completion and yields the result.
  detail::awaitable_as_function<T> wrapper(std::move(a));
  auto st = std::make_shared<detail::spawn_wait_state<T>>(ex);

  auto state = std::make_shared<detail::spawn_state<T>>(std::move(wrapper));
  auto entry = detail::spawn_entry_point<T>(std::move(state));

  co_spawn(ex, detail::run_to_state<T>(ex, st, std::move(entry)), detached);
  return detail::await_state<T>(std::move(st));
}

/// Start an awaitable on the given thread_pool_executor, returning an awaitable.
template <typename T>
auto co_spawn(thread_pool_executor pex, awaitable<T> a, use_awaitable_t) -> awaitable<T> {
  auto ex = pex.pick_executor();
  co_return co_await co_spawn(ex, std::move(a), use_awaitable);
}

/// Start a callable that returns iocoro::awaitable<T> on the given executor, returning an
/// awaitable that can be awaited to obtain the result.
template <typename F>
  requires detail::awaitable_factory<std::remove_cvref_t<F>>
auto co_spawn(executor ex, F&& f, use_awaitable_t)
  -> awaitable<detail::awaitable_value_t<std::remove_cvref_t<F>>> {
  using value_type = detail::awaitable_value_t<std::remove_cvref_t<F>>;

  // Preserve hot-start semantics (tests rely on it): start immediately (once the context runs),
  // and return an awaitable that only waits for completion and yields the result.
  auto st = std::make_shared<detail::spawn_wait_state<value_type>>(ex);

  auto state = std::make_shared<detail::spawn_state<value_type>>(std::forward<F>(f));
  auto entry = detail::spawn_entry_point<value_type>(std::move(state));

  co_spawn(ex, detail::run_to_state<value_type>(ex, st, std::move(entry)), detached);
  return detail::await_state<value_type>(std::move(st));
}

/// Start a callable that returns iocoro::awaitable<T> on the given thread_pool_executor,
/// returning an awaitable that can be awaited to obtain the result.
template <typename F>
  requires detail::awaitable_factory<std::remove_cvref_t<F>>
auto co_spawn(thread_pool_executor pex, F&& f, use_awaitable_t)
  -> awaitable<detail::awaitable_value_t<std::remove_cvref_t<F>>> {
  auto ex = pex.pick_executor();
  co_return co_await co_spawn(ex, std::forward<F>(f), use_awaitable);
}

/// Start an awaitable on the given executor, invoking a completion callback with either
/// the result or an exception.
template <typename T, typename F>
  requires detail::completion_callback_for<F, T>
void co_spawn(executor ex, awaitable<T> a, F&& completion) {
  detail::awaitable_as_function<T> wrapper(std::move(a));

  auto state =
    std::make_shared<detail::spawn_state_with_completion<T>>(std::move(wrapper),
                                                            std::forward<F>(completion));
  auto entry = detail::spawn_entry_point_with_completion<T>(std::move(state));

  detail::spawn_detached_impl(ex, std::move(entry));
}

/// Start an awaitable on the given thread_pool_executor, invoking a completion callback.
template <typename T, typename F>
  requires detail::completion_callback_for<F, T>
void co_spawn(thread_pool_executor pex, awaitable<T> a, F&& completion) {
  auto ex = pex.pick_executor();
  co_spawn(ex, std::move(a), std::forward<F>(completion));
}

/// Start a callable that returns iocoro::awaitable<T> on the given executor, invoking a
/// completion callback with either the result or an exception.
template <typename Factory, typename Completion>
  requires detail::awaitable_factory<std::remove_cvref_t<Factory>> &&
           detail::completion_callback_for<std::remove_cvref_t<Completion>,
                                           detail::awaitable_value_t<std::remove_cvref_t<Factory>>>
void co_spawn(executor ex, Factory&& f, Completion&& completion) {
  using value_type = detail::awaitable_value_t<std::remove_cvref_t<Factory>>;

  auto state = std::make_shared<detail::spawn_state_with_completion<value_type>>(
    std::forward<Factory>(f), std::forward<Completion>(completion));
  auto entry = detail::spawn_entry_point_with_completion<value_type>(std::move(state));

  detail::spawn_detached_impl(ex, std::move(entry));
}

/// Start a callable that returns iocoro::awaitable<T> on the given thread_pool_executor,
/// invoking a completion callback with either the result or an exception.
template <typename Factory, typename Completion>
  requires detail::awaitable_factory<std::remove_cvref_t<Factory>> &&
           detail::completion_callback_for<std::remove_cvref_t<Completion>,
                                           detail::awaitable_value_t<std::remove_cvref_t<Factory>>>
void co_spawn(thread_pool_executor pex, Factory&& f, Completion&& completion) {
  auto ex = pex.pick_executor();
  co_spawn(ex, std::forward<Factory>(f), std::forward<Completion>(completion));
}

}  // namespace iocoro

#pragma once

#include <iocoro/completion_token.hpp>
#include <iocoro/detail/spawn.hpp>
#include <iocoro/executor.hpp>
#include <iocoro/io_executor.hpp>
#include <iocoro/thread_pool_executor.hpp>
#include <iocoro/traits/awaitable_result.hpp>

namespace iocoro {

/// Extract the value type `T` from a callable returning `iocoro::awaitable<T>`.
///
/// This alias is intentionally ill-formed if `F()` does not return
/// `iocoro::awaitable<T>`, so that misuse is diagnosed at the concept boundary.
template <typename F>
using awaitable_factory_result_t = traits::awaitable_result_t<std::invoke_result_t<F&>>;

/// A callable that can be invoked with no arguments and returns
/// `iocoro::awaitable<T>` for some `T`.
template <typename F>
concept awaitable_factory =
  std::invocable<F&> && requires { typename awaitable_factory_result_t<F>; };

/// Start a callable that returns iocoro::awaitable<T> on the given executor (detached).
template <typename F>
  requires awaitable_factory<std::remove_cvref_t<F>>
void co_spawn(any_executor ex, F&& f, detached_t) {
  using value_type = awaitable_factory_result_t<std::remove_cvref_t<F>>;

  auto state = std::make_shared<detail::spawn_state<value_type>>(std::forward<F>(f));
  auto entry = detail::spawn_entry_point<value_type>(std::move(state));

  detail::spawn_detached_impl(std::move(ex), std::move(entry));
}

/// Start a callable that returns iocoro::awaitable<T> on the given executor, returning an
/// awaitable that can be awaited to obtain the result.
template <typename F>
  requires awaitable_factory<std::remove_cvref_t<F>>
auto co_spawn(any_executor ex, F&& f, use_awaitable_t)
  -> awaitable<awaitable_factory_result_t<std::remove_cvref_t<F>>> {
  using value_type = awaitable_factory_result_t<std::remove_cvref_t<F>>;

  auto st = std::make_shared<detail::spawn_wait_state<value_type>>(ex);

  auto state = std::make_shared<detail::spawn_state<value_type>>(std::forward<F>(f));
  auto entry = detail::spawn_entry_point<value_type>(std::move(state));

  co_spawn(ex, detail::run_to_state<value_type>(ex, st, std::move(entry)), detached);
  return detail::await_state<value_type>(std::move(st));
}

/// Start a callable that returns iocoro::awaitable<T> on the given executor, invoking a
/// completion callback with either the result or an exception.
template <typename F, typename Completion>
  requires awaitable_factory<std::remove_cvref_t<F>> &&
           detail::completion_callback_for<std::remove_cvref_t<Completion>,
                                           awaitable_factory_result_t<std::remove_cvref_t<F>>>
void co_spawn(any_executor ex, F&& f, Completion&& completion) {
  using value_type = awaitable_factory_result_t<std::remove_cvref_t<F>>;

  auto state = std::make_shared<detail::spawn_state_with_completion<value_type>>(
    std::forward<F>(f), std::forward<Completion>(completion));
  auto entry = detail::spawn_entry_point_with_completion<value_type>(std::move(state));

  detail::spawn_detached_impl(std::move(ex), std::move(entry));
}

/// Overload for awaitable<T>: converts to factory lambda and forwards to unified implementation.
template <typename T, typename Token>
auto co_spawn(any_executor ex, awaitable<T> a, Token&& token) {
  return co_spawn(
    ex, [a = std::move(a)]() mutable -> awaitable<T> { return std::move(a); },
    std::forward<Token>(token));
}

/// Generic forwarding overload for io_executor: converts to any_executor and forwards all
/// arguments.
template <typename... Args>
auto co_spawn(io_executor ex, Args&&... args)
  -> decltype(co_spawn(std::declval<any_executor>(), std::declval<Args>()...)) {
  return co_spawn(any_executor{ex}, std::forward<Args>(args)...);
}

/// Generic forwarding overload for thread_pool_executor: picks an executor and forwards all
/// arguments.
template <typename... Args>
auto co_spawn(thread_pool_executor pex, Args&&... args)
  -> decltype(co_spawn(std::declval<io_executor>(), std::declval<Args>()...)) {
  return co_spawn(pex.pick_executor(), std::forward<Args>(args)...);
}

}  // namespace iocoro

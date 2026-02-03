#pragma once

#include <iocoro/any_executor.hpp>
#include <iocoro/completion_token.hpp>
#include <iocoro/detail/spawn.hpp>
#include <iocoro/expected.hpp>
#include <iocoro/traits/awaitable_result.hpp>

#include <concepts>
#include <functional>
#include <memory>
#include <stop_token>
#include <type_traits>
#include <utility>

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

template <typename F, typename T>
concept completion_callback_for = std::invocable<F&, expected<T, std::exception_ptr>> &&
                                  (!std::same_as<std::remove_cvref_t<F>, detached_t>) &&
                                  (!std::same_as<std::remove_cvref_t<F>, use_awaitable_t>);

/// Start an `awaitable` factory on `ex`.
///
/// Overloads:
/// - `detached`: fire-and-forget; exceptions are swallowed.
/// - `use_awaitable`: returns an `awaitable` that yields the result (or rethrows).
/// - completion callback: called with `expected<T, exception_ptr>`; callback exceptions are swallowed.
///
/// IMPORTANT: The coroutine is *started* by posting its first `resume()` onto `ex`.
/// There is no guarantee of inline execution at the call site.
namespace detail {

template <typename F, typename Completion>
  requires awaitable_factory<std::remove_cvref_t<F>>
void spawn_with_completion(spawn_context ctx, F&& f, Completion&& completion) {
  using value_type = awaitable_factory_result_t<std::remove_cvref_t<F>>;
  spawn_task<value_type>(std::move(ctx), std::forward<F>(f), std::forward<Completion>(completion));
}

template <typename F, typename Completion>
  requires awaitable_factory<std::remove_cvref_t<F>>
void spawn_with_completion(any_executor ex, F&& f, Completion&& completion) {
  spawn_with_completion(spawn_context{std::move(ex)}, std::forward<F>(f),
                        std::forward<Completion>(completion));
}

}  // namespace detail

template <typename F>
  requires awaitable_factory<std::remove_cvref_t<F>>
void co_spawn(any_executor ex, F&& f, detached_t) {
  using value_type = awaitable_factory_result_t<std::remove_cvref_t<F>>;
  detail::spawn_with_completion(std::move(ex), std::forward<F>(f),
                                detail::detached_completion<value_type>{});
}

template <typename F>
  requires awaitable_factory<std::remove_cvref_t<F>>
void co_spawn(any_executor ex, std::stop_token stop_token, F&& f, detached_t) {
  using value_type = awaitable_factory_result_t<std::remove_cvref_t<F>>;
  detail::spawn_with_completion(detail::spawn_context{std::move(ex), stop_token},
                                std::forward<F>(f), detail::detached_completion<value_type>{});
}

/// Start a factory on `ex` and return an awaitable for its result.
///
/// IMPORTANT: This does not automatically cancel the spawned coroutine if the awaiting
/// coroutine is stopped or destroyed; it only controls how the result is delivered.
template <typename F>
  requires awaitable_factory<std::remove_cvref_t<F>>
[[nodiscard]] auto co_spawn(any_executor ex, F&& f, use_awaitable_t)
  -> awaitable<awaitable_factory_result_t<std::remove_cvref_t<F>>> {
  using value_type = awaitable_factory_result_t<std::remove_cvref_t<F>>;

  auto st = std::make_shared<detail::spawn_result_state<value_type>>();
  detail::spawn_with_completion(std::move(ex), std::forward<F>(f),
                                detail::result_state_completion<value_type>{st});
  return detail::await_result<value_type>(std::move(st));
}

template <typename F>
  requires awaitable_factory<std::remove_cvref_t<F>>
[[nodiscard]] auto co_spawn(any_executor ex, std::stop_token stop_token, F&& f, use_awaitable_t)
  -> awaitable<awaitable_factory_result_t<std::remove_cvref_t<F>>> {
  using value_type = awaitable_factory_result_t<std::remove_cvref_t<F>>;

  auto st = std::make_shared<detail::spawn_result_state<value_type>>();
  detail::spawn_with_completion(detail::spawn_context{std::move(ex), stop_token},
                                std::forward<F>(f),
                                detail::result_state_completion<value_type>{st});
  return detail::await_result<value_type>(std::move(st));
}

/// Start a factory on `ex` and invoke `completion` when done.
///
/// `completion` is invoked with:
/// - `expected<T, exception_ptr>{value}` on success
/// - `unexpected(exception_ptr)` if the coroutine throws
template <typename F, typename Completion>
  requires awaitable_factory<std::remove_cvref_t<F>> &&
           completion_callback_for<std::remove_cvref_t<Completion>,
                                   awaitable_factory_result_t<std::remove_cvref_t<F>>>
void co_spawn(any_executor ex, F&& f, Completion&& completion) {
  detail::spawn_with_completion(std::move(ex), std::forward<F>(f),
                                std::forward<Completion>(completion));
}

template <typename F, typename Completion>
  requires awaitable_factory<std::remove_cvref_t<F>> &&
           completion_callback_for<std::remove_cvref_t<Completion>,
                                   awaitable_factory_result_t<std::remove_cvref_t<F>>>
void co_spawn(any_executor ex, std::stop_token stop_token, F&& f, Completion&& completion) {
  detail::spawn_with_completion(detail::spawn_context{std::move(ex), stop_token},
                                std::forward<F>(f), std::forward<Completion>(completion));
}

/// Convenience overload: treat an `awaitable<T>` as a factory and forward.
template <typename T, typename Token>
auto co_spawn(any_executor ex, awaitable<T> a, Token&& token) {
  return co_spawn(
    ex, [a = std::move(a)]() mutable -> awaitable<T> { return std::move(a); },
    std::forward<Token>(token));
}

template <typename T, typename Token>
auto co_spawn(any_executor ex, std::stop_token stop_token, awaitable<T> a, Token&& token) {
  return co_spawn(
    std::move(ex), stop_token,
    [a = std::move(a)]() mutable -> awaitable<T> { return std::move(a); },
    std::forward<Token>(token));
}

}  // namespace iocoro

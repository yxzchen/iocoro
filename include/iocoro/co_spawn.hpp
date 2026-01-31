#pragma once

#include <iocoro/completion_token.hpp>
#include <iocoro/detail/spawn.hpp>
#include <iocoro/any_executor.hpp>
#include <iocoro/traits/awaitable_result.hpp>

#include <stop_token>

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

/// Start a callable that returns iocoro::awaitable<T> on the given executor (detached).
template <typename F>
  requires awaitable_factory<std::remove_cvref_t<F>>
void co_spawn(any_executor ex, F&& f, detached_t) {
  co_spawn(std::move(ex), std::stop_token{}, std::forward<F>(f), detached);
}

template <typename F>
  requires awaitable_factory<std::remove_cvref_t<F>>
void co_spawn(any_executor ex, std::stop_token parent_tok, F&& f, detached_t) {
  using value_type = awaitable_factory_result_t<std::remove_cvref_t<F>>;

  auto ctx = detail::make_spawn_context(std::move(ex), std::move(parent_tok));
  detail::spawn_task<value_type>(std::move(ctx), std::forward<F>(f),
                                 detail::detached_completion<value_type>{});
}

/// Start a callable that returns iocoro::awaitable<T> on the given executor, returning an
/// awaitable that can be awaited to obtain the result.
template <typename F>
  requires awaitable_factory<std::remove_cvref_t<F>>
auto co_spawn(any_executor ex, F&& f, use_awaitable_t)
  -> awaitable<awaitable_factory_result_t<std::remove_cvref_t<F>>> {
  return co_spawn(std::move(ex), std::stop_token{}, std::forward<F>(f), use_awaitable);
}

template <typename F>
  requires awaitable_factory<std::remove_cvref_t<F>>
auto co_spawn(any_executor ex, std::stop_token parent_tok, F&& f, use_awaitable_t)
  -> awaitable<awaitable_factory_result_t<std::remove_cvref_t<F>>> {
  using value_type = awaitable_factory_result_t<std::remove_cvref_t<F>>;

  auto st = std::make_shared<detail::spawn_result_state<value_type>>();
  auto ctx = detail::make_spawn_context(std::move(ex), std::move(parent_tok));

  detail::spawn_task<value_type>(std::move(ctx), std::forward<F>(f),
                                 detail::result_state_completion<value_type>{st});
  return detail::await_result<value_type>(std::move(st));
}

/// Start a callable that returns iocoro::awaitable<T> on the given executor, invoking a
/// completion callback with either the result or an exception.
template <typename F, typename Completion>
  requires awaitable_factory<std::remove_cvref_t<F>> &&
           completion_callback_for<std::remove_cvref_t<Completion>,
                                   awaitable_factory_result_t<std::remove_cvref_t<F>>>
void co_spawn(any_executor ex, F&& f, Completion&& completion) {
  co_spawn(std::move(ex), std::stop_token{}, std::forward<F>(f), std::forward<Completion>(completion));
}

template <typename F, typename Completion>
  requires awaitable_factory<std::remove_cvref_t<F>> &&
           completion_callback_for<std::remove_cvref_t<Completion>,
                                   awaitable_factory_result_t<std::remove_cvref_t<F>>>
void co_spawn(any_executor ex, std::stop_token parent_tok, F&& f, Completion&& completion) {
  using value_type = awaitable_factory_result_t<std::remove_cvref_t<F>>;

  auto ctx = detail::make_spawn_context(std::move(ex), std::move(parent_tok));
  detail::spawn_task<value_type>(std::move(ctx), std::forward<F>(f),
                                 std::forward<Completion>(completion));
}

/// Overload for awaitable<T>: converts to factory lambda and forwards to unified implementation.
template <typename T, typename Token>
auto co_spawn(any_executor ex, awaitable<T> a, Token&& token) {
  return co_spawn(
    ex, [a = std::move(a)]() mutable -> awaitable<T> { return std::move(a); },
    std::forward<Token>(token));
}

template <typename T, typename Token>
auto co_spawn(any_executor ex, std::stop_token parent_tok, awaitable<T> a, Token&& token) {
  return co_spawn(
    ex, std::move(parent_tok),
    [a = std::move(a)]() mutable -> awaitable<T> { return std::move(a); },
    std::forward<Token>(token));
}

}  // namespace iocoro

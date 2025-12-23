#pragma once

#include <xz/io/awaitable.hpp>

#include <xz/io/assert.hpp>
#include <xz/io/co_spawn.hpp>
#include <xz/io/detached.hpp>
#include <xz/io/detail/when/when_all_state.hpp>
#include <xz/io/this_coro.hpp>

#include <cstddef>
#include <exception>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace xz::io {

namespace detail {

// Bind executor to an awaitable
template <typename T>
auto when_all_bind_executor(executor ex, awaitable<T>&& a) -> awaitable<T> {
  auto h = a.release();
  h.promise().set_executor(ex);
  return awaitable<T>{h};
}

// Runner coroutine for variadic when_all
template <std::size_t I, class T, class... Ts>
auto when_all_run_one(executor ex, std::shared_ptr<when_all_variadic_state<Ts...>> st, awaitable<T> a)
  -> awaitable<void> {
  auto bound = when_all_bind_executor<T>(ex, std::move(a));
  try {
    if constexpr (std::is_void_v<T>) {
      co_await std::move(bound);
      st->template set_value<I>(std::monostate{});
    } else {
      st->template set_value<I>(co_await std::move(bound));
    }
  } catch (...) {
    st->set_exception(std::current_exception());
  }
  if (st->try_complete()) {
    st->complete();
  }
}

template <class... Ts, std::size_t... Is>
void when_all_start_variadic([[maybe_unused]] executor ex,
                             [[maybe_unused]] std::shared_ptr<when_all_variadic_state<Ts...>> st,
                             [[maybe_unused]] std::tuple<awaitable<Ts>...> tasks,
                             std::index_sequence<Is...>) {
  (co_spawn(ex,
            when_all_run_one<Is, std::tuple_element_t<Is, std::tuple<Ts...>>, Ts...>(
              ex, st, std::move(std::get<Is>(tasks))),
            detached),
   ...);
}

template <class... Ts, std::size_t... Is>
auto when_all_collect_variadic([[maybe_unused]] typename when_all_variadic_state<Ts...>::values_tuple values,
                               std::index_sequence<Is...>) -> std::tuple<when_value_t<Ts>...> {
  return std::tuple<when_value_t<Ts>...>{
    ([&]() -> when_value_t<std::tuple_element_t<Is, std::tuple<Ts...>>> {
      auto& opt = std::get<Is>(values);
      XZ_ENSURE(opt.has_value(), "when_all: missing value");
      return std::move(*opt);
    })()...};
}

// Runner coroutine for container when_all
template <class T>
auto when_all_container_run_one(executor ex, std::shared_ptr<when_all_container_state<T>> st,
                                std::size_t i, awaitable<T> a) -> awaitable<void> {
  auto bound = when_all_bind_executor<T>(ex, std::move(a));
  try {
    if constexpr (std::is_void_v<T>) {
      co_await std::move(bound);
    } else {
      st->set_value(i, co_await std::move(bound));
    }
  } catch (...) {
    st->set_exception(std::current_exception());
  }
  if (st->try_complete()) {
    st->complete();
  }
}

}  // namespace detail

/// Wait for all awaitables to complete (variadic).
///
/// Semantics:
/// - All tasks are started concurrently on the current coroutine's executor.
/// - The returned awaitable completes once all tasks finished.
/// - If any task throws, when_all waits for all tasks and then rethrows the first exception.
/// - void results are represented as std::monostate in the returned tuple.
template <class... Ts>
auto when_all(awaitable<Ts>... tasks)
  -> awaitable<std::tuple<::xz::io::detail::when_value_t<Ts>...>> {
  auto ex = co_await this_coro::executor;
  XZ_ENSURE(ex, "when_all: requires a bound executor");

  if constexpr (sizeof...(Ts) == 0) {
    co_return std::tuple<::xz::io::detail::when_value_t<Ts>...>{};
  }

  auto st = std::make_shared<::xz::io::detail::when_all_variadic_state<Ts...>>(ex);
  detail::when_all_start_variadic<Ts...>(ex, st, std::tuple<awaitable<Ts>...>{std::move(tasks)...},
                                         std::index_sequence_for<Ts...>{});

  co_await ::xz::io::detail::await_when(st);

  std::exception_ptr ep{};
  typename ::xz::io::detail::when_all_variadic_state<Ts...>::values_tuple values{};
  {
    std::scoped_lock lk{st->m};
    ep = st->first_ep;
    if (!ep) {
      values = std::move(st->values);
    }
  }

  if (ep) {
    std::rethrow_exception(ep);
  }
  co_return detail::when_all_collect_variadic<Ts...>(std::move(values),
                                                     std::index_sequence_for<Ts...>{});
}

/// Wait for all awaitables to complete (container).
///
/// Semantics are the same as the variadic overload.
template <class T>
auto when_all(std::vector<awaitable<T>> tasks)
  -> awaitable<std::conditional_t<std::is_void_v<T>, void, std::vector<std::remove_cvref_t<T>>>> {
  auto ex = co_await this_coro::executor;
  XZ_ENSURE(ex, "when_all(vector): requires a bound executor");

  if (tasks.empty()) {
    if constexpr (std::is_void_v<T>) {
      co_return;
    } else {
      co_return std::vector<std::remove_cvref_t<T>>{};
    }
  }

  auto st = std::make_shared<::xz::io::detail::when_all_container_state<T>>(ex, tasks.size());

  for (std::size_t i = 0; i < tasks.size(); ++i) {
    co_spawn(ex, detail::when_all_container_run_one<T>(ex, st, i, std::move(tasks[i])), detached);
  }

  co_await ::xz::io::detail::await_when(st);

  std::exception_ptr ep{};
  std::vector<std::optional<::xz::io::detail::when_value_t<T>>> values{};
  {
    std::scoped_lock lk{st->m};
    ep = st->first_ep;
    if (!ep && !std::is_void_v<T>) {
      values = std::move(st->values);
    }
  }

  if (ep) {
    std::rethrow_exception(ep);
  }

  if constexpr (std::is_void_v<T>) {
    co_return;
  } else {
    std::vector<std::remove_cvref_t<T>> out(tasks.size());
    for (std::size_t i = 0; i < out.size(); ++i) {
      XZ_ENSURE(i < values.size(), "when_all(vector): internal index out of range");
      XZ_ENSURE(values[i].has_value(), "when_all(vector): missing value");
      out[i] = std::move(*values[i]);
    }
    co_return out;
  }
}

}  // namespace xz::io

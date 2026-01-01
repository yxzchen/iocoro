#pragma once

#include <iocoro/awaitable.hpp>

#include <iocoro/assert.hpp>
#include <iocoro/co_spawn.hpp>
#include <iocoro/completion_token.hpp>
#include <iocoro/detail/when/when_all_state.hpp>
#include <iocoro/this_coro.hpp>

#include <cstddef>
#include <exception>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace iocoro {

namespace detail {

// Runner coroutine for variadic when_all
template <std::size_t I, class T, class... Ts>
auto when_all_run_one(std::shared_ptr<when_all_variadic_state<Ts...>> st, awaitable<T> a)
  -> awaitable<void> {
  try {
    if constexpr (std::is_void_v<T>) {
      co_await std::move(a);
      st->template set_value<I>(std::monostate{});
    } else {
      st->template set_value<I>(co_await std::move(a));
    }
  } catch (...) {
    st->set_exception(std::current_exception());
  }
  if (st->try_complete()) {
    st->complete();
  }
}

template <class... Ts, std::size_t... Is>
void when_all_start_variadic(any_executor fallback_ex,
                             [[maybe_unused]] std::shared_ptr<when_all_variadic_state<Ts...>> st,
                             [[maybe_unused]] std::tuple<awaitable<Ts>...> tasks,
                             std::index_sequence<Is...>) {
  (co_spawn([&]() {
              auto task_ex = std::get<Is>(tasks).get_executor();
              return task_ex ? task_ex : fallback_ex;
            }(),
            when_all_run_one<Is, std::tuple_element_t<Is, std::tuple<Ts...>>, Ts...>(
              st, std::move(std::get<Is>(tasks))),
            detached),
   ...);
}

template <class... Ts, std::size_t... Is>
auto when_all_collect_variadic([[maybe_unused]]
                               typename when_all_variadic_state<Ts...>::values_tuple values,
                               std::index_sequence<Is...>) -> std::tuple<when_value_t<Ts>...> {
  return std::tuple<when_value_t<Ts>...>{
    ([&]() -> when_value_t<std::tuple_element_t<Is, std::tuple<Ts...>>> {
      auto& opt = std::get<Is>(values);
      IOCORO_ENSURE(opt.has_value(), "when_all: missing value");
      return std::move(*opt);
    })()...};
}

// Runner coroutine for container when_all
template <class T>
auto when_all_container_run_one(std::shared_ptr<when_all_container_state<T>> st, std::size_t i,
                                awaitable<T> a) -> awaitable<void> {
  try {
    if constexpr (std::is_void_v<T>) {
      co_await std::move(a);
    } else {
      st->set_value(i, co_await std::move(a));
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
/// - All tasks are started concurrently, each on its own bound executor.
/// - If a task doesn't have a bound executor, it uses the calling coroutine's executor.
/// - The returned awaitable completes once all tasks finished.
/// - If any task throws, when_all waits for all tasks and then rethrows the first exception.
/// - void results are represented as std::monostate in the returned tuple.
template <class... Ts>
auto when_all(awaitable<Ts>... tasks) -> awaitable<std::tuple<detail::when_value_t<Ts>...>> {
  if constexpr (sizeof...(Ts) == 0) {
    co_return std::tuple<detail::when_value_t<Ts>...>{};
  }

  auto fallback_ex = co_await this_coro::executor;
  IOCORO_ENSURE(fallback_ex, "when_all: requires a bound executor");

  auto st = std::make_shared<detail::when_all_variadic_state<Ts...>>();
  detail::when_all_start_variadic<Ts...>(fallback_ex, st, std::tuple<awaitable<Ts>...>{std::move(tasks)...},
                                         std::index_sequence_for<Ts...>{});

  co_await detail::await_when(st);

  std::exception_ptr ep{};
  typename detail::when_all_variadic_state<Ts...>::values_tuple values{};
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
  if (tasks.empty()) {
    if constexpr (std::is_void_v<T>) {
      co_return;
    } else {
      co_return std::vector<std::remove_cvref_t<T>>{};
    }
  }

  auto fallback_ex = co_await this_coro::executor;
  IOCORO_ENSURE(fallback_ex, "when_all(vector): requires a bound executor");

  auto st = std::make_shared<detail::when_all_container_state<T>>(tasks.size());

  for (std::size_t i = 0; i < tasks.size(); ++i) {
    auto task_executor = tasks[i].get_executor();
    auto exec = task_executor ? task_executor : fallback_ex;
    co_spawn(exec, detail::when_all_container_run_one<T>(st, i, std::move(tasks[i])), detached);
  }

  co_await detail::await_when(st);

  std::exception_ptr ep{};
  std::vector<std::optional<detail::when_value_t<T>>> values{};
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
      IOCORO_ENSURE(i < values.size(), "when_all(vector): internal index out of range");
      IOCORO_ENSURE(values[i].has_value(), "when_all(vector): missing value");
      out[i] = std::move(*values[i]);
    }
    co_return out;
  }
}

}  // namespace iocoro

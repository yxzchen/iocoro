#pragma once

#include <iocoro/awaitable.hpp>

#include <iocoro/assert.hpp>
#include <iocoro/co_spawn.hpp>
#include <iocoro/completion_token.hpp>
#include <iocoro/detail/when/when_any_state.hpp>
#include <iocoro/this_coro.hpp>

#include <cstddef>
#include <exception>
#include <optional>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace iocoro {

namespace detail {

// Runner coroutine for variadic when_any
template <std::size_t I, class T, class... Ts>
auto when_any_run_one(std::shared_ptr<when_any_variadic_state<Ts...>> st,
                      awaitable<T> a) -> awaitable<void> {
  try {
    if constexpr (std::is_void_v<T>) {
      co_await std::move(a);
      if (st->try_complete()) {
        st->template set_value<I>(std::monostate{});
        st->complete();
      }
    } else {
      auto result = co_await std::move(a);
      if (st->try_complete()) {
        st->template set_value<I>(std::move(result));
        st->complete();
      }
    }
  } catch (...) {
    if (st->try_complete()) {
      st->set_exception(std::current_exception());
      st->complete();
    }
  }
}

template <class... Ts, std::size_t... Is>
void when_any_start_variadic(any_executor fallback_ex, std::stop_token parent_stop,
                             [[maybe_unused]] std::shared_ptr<when_any_variadic_state<Ts...>> st,
                             [[maybe_unused]] std::tuple<awaitable<Ts>...> tasks,
                             std::index_sequence<Is...>) {
  (
    [&]() {
      auto task = std::move(std::get<Is>(tasks));
      auto const task_ex = task.get_executor();
      auto const exec = task_ex ? task_ex : fallback_ex;
      detail::spawn_task<void>(
        detail::spawn_context{exec, parent_stop},
        [st, task = std::move(task)]() mutable -> awaitable<void> {
          return when_any_run_one<Is, std::tuple_element_t<Is, std::tuple<Ts...>>, Ts...>(
            st, std::move(task));
        },
        detail::detached_completion<void>{});
    }(),
    ...);
}

template <class... Ts, std::size_t... Is>
auto when_any_collect_variadic(std::size_t index,
                               typename when_any_variadic_state<Ts...>::values_variant result,
                               std::index_sequence<Is...>) -> std::variant<when_value_t<Ts>...> {
  std::variant<when_value_t<Ts>...> out;
  bool found =
    ((index == Is ? (out.template emplace<Is>(
                       [&]() -> when_value_t<std::tuple_element_t<Is, std::tuple<Ts...>>> {
                         auto& opt = std::get<Is + 1>(result);
                         IOCORO_ENSURE(opt.has_value(), "when_any: missing value");
                         return std::move(*opt);
                       }()),
                     true)
                  : false) ||
     ...);
  IOCORO_ENSURE(found, "when_any: invalid completed index");
  return out;
}

// Runner coroutine for container when_any
template <class T>
auto when_any_container_run_one(std::shared_ptr<when_any_container_state<T>> st, std::size_t i,
                                awaitable<T> a) -> awaitable<void> {
  try {
    if constexpr (std::is_void_v<T>) {
      co_await std::move(a);
      if (st->try_complete()) {
        st->set_void_result(i);
        st->complete();
      }
    } else {
      auto result = co_await std::move(a);
      if (st->try_complete()) {
        st->set_value(i, std::move(result));
        st->complete();
      }
    }
  } catch (...) {
    if (st->try_complete()) {
      st->set_exception(std::current_exception());
      st->complete();
    }
  }
}

}  // namespace detail

/// Wait for any awaitable to complete (variadic).
///
/// Semantics:
/// - All tasks are started concurrently, each on its own bound executor.
/// - If a task doesn't have a bound executor, it uses the calling coroutine's executor.
/// - The returned awaitable completes once the first task finishes.
/// - Returns a variant containing the result of the first completed task.
/// - If the first task throws, when_any rethrows the exception.
/// - void results are represented as std::monostate in the variant.
/// - Other tasks may still be running after when_any returns.
template <class... Ts>
auto when_any(awaitable<Ts>... tasks)
  -> awaitable<std::pair<std::size_t, std::variant<detail::when_value_t<Ts>...>>> {
  static_assert(sizeof...(Ts) > 0, "when_any requires at least one task");

  auto fallback_ex = co_await this_coro::executor;
  IOCORO_ENSURE(fallback_ex, "when_any: requires a bound executor");
  auto parent_stop = co_await this_coro::stop_token;

  auto st = std::make_shared<detail::when_any_variadic_state<Ts...>>();
  auto tasks_tuple = std::tuple<awaitable<Ts>...>{std::move(tasks)...};
  detail::when_any_start_variadic<Ts...>(fallback_ex, parent_stop, st, std::move(tasks_tuple),
                                         std::index_sequence_for<Ts...>{});

  co_await detail::await_when(st);

  std::exception_ptr ep{};
  std::size_t index{};
  typename detail::when_any_variadic_state<Ts...>::values_variant result{};
  {
    std::scoped_lock lk{st->m};
    ep = st->first_ep;
    if (!ep) {
      index = st->completed_index;
      result = std::move(st->result);
    }
  }

  if (ep) {
    std::rethrow_exception(ep);
  }

  co_return std::make_pair(index, detail::when_any_collect_variadic<Ts...>(
                                    index, std::move(result), std::index_sequence_for<Ts...>{}));
}

/// Wait for any awaitable to complete (container).
///
/// Semantics are similar to the variadic overload.
/// Returns the index and value of the first completed task.
template <class T>
auto when_any(std::vector<awaitable<T>> tasks)
  -> awaitable<std::pair<
    std::size_t, std::conditional_t<std::is_void_v<T>, std::monostate, std::remove_cvref_t<T>>>> {
  IOCORO_ENSURE(!tasks.empty(), "when_any(vector): requires at least one task");

  auto fallback_ex = co_await this_coro::executor;
  IOCORO_ENSURE(fallback_ex, "when_any(vector): requires a bound executor");
  auto parent_stop = co_await this_coro::stop_token;

  auto st = std::make_shared<detail::when_any_container_state<T>>();

  for (std::size_t i = 0; i < tasks.size(); ++i) {
    auto task_executor = tasks[i].get_executor();
    auto exec = task_executor ? task_executor : fallback_ex;
    detail::spawn_task<void>(
      detail::spawn_context{exec, parent_stop},
      [st, i, task = std::move(tasks[i])]() mutable -> awaitable<void> {
        return detail::when_any_container_run_one<T>(st, i, std::move(task));
      },
      detail::detached_completion<void>{});
  }

  co_await detail::await_when(st);

  std::exception_ptr ep{};
  std::size_t index{};
  std::optional<std::remove_cvref_t<T>> result{};
  {
    std::scoped_lock lk{st->m};
    ep = st->first_ep;
    if (!ep) {
      index = st->completed_index;
      if constexpr (!std::is_void_v<T>) {
        result = std::move(st->result);
      }
    }
  }

  if (ep) {
    std::rethrow_exception(ep);
  }

  if constexpr (std::is_void_v<T>) {
    co_return std::make_pair(index, std::monostate{});
  } else {
    IOCORO_ENSURE(result.has_value(), "when_any(vector): missing value");
    co_return std::make_pair(index, std::move(*result));
  }
}

}  // namespace iocoro

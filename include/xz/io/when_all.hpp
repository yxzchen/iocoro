#pragma once

#include <xz/io/awaitable.hpp>

#include <xz/io/assert.hpp>
#include <xz/io/co_spawn.hpp>
#include <xz/io/detail/when_all/container_state.hpp>
#include <xz/io/detail/when_all/state.hpp>
#include <xz/io/this_coro.hpp>

#include <cstddef>
#include <exception>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace xz::io {

namespace detail {

template <std::size_t I, class T, class... Ts>
void when_all_spawn_one(executor ex, std::shared_ptr<when_all_state<Ts...>> st, awaitable<T> a) {
  // Start task concurrently and report completion via callback (no use_awaitable/co_spawn hot-waiters).
  co_spawn(ex, std::move(a), [st = std::move(st)](auto r) mutable {
    try {
      if constexpr (std::is_void_v<T>) {
        if (r.has_value()) {
          st->template set_value<I>(std::monostate{});
        } else {
          st->set_exception(std::move(r).error());
        }
      } else {
        if (r.has_value()) {
          st->template set_value<I>(std::move(r).value());
        } else {
          st->set_exception(std::move(r).error());
        }
      }
    } catch (...) {
      st->set_exception(std::current_exception());
    }
    st->arrive();
  });
}

template <class... Ts, std::size_t... Is>
void when_all_start_variadic(executor ex,
                            std::shared_ptr<when_all_state<Ts...>> st,
                            std::tuple<awaitable<Ts>...> tasks,
                            std::index_sequence<Is...>) {
  (when_all_spawn_one<Is, std::tuple_element_t<Is, std::tuple<Ts...>>, Ts...>(
     ex, st, std::move(std::get<Is>(tasks))),
   ...);
}

template <class... Ts, std::size_t... Is>
auto when_all_collect_variadic(typename when_all_state<Ts...>::values_tuple values,
                               std::index_sequence<Is...>)
  -> std::tuple<when_all_value_t<Ts>...> {
  return std::tuple<when_all_value_t<Ts>...>{
    ([&]() -> when_all_value_t<std::tuple_element_t<Is, std::tuple<Ts...>>> {
      auto& opt = std::get<Is>(values);
      XZ_ENSURE(opt.has_value(), "when_all: missing value");
      return std::move(*opt);
    })()...};
}

template <class T>
void when_all_container_spawn_one(executor ex,
                                 std::shared_ptr<when_all_container_state<T>> st,
                                 std::size_t i,
                                 awaitable<T> a) {
  co_spawn(ex, std::move(a), [st = std::move(st), i](auto r) mutable {
    try {
      if constexpr (std::is_void_v<T>) {
        if (!r.has_value()) {
          st->set_exception(std::move(r).error());
        }
      } else {
        if (r.has_value()) {
          st->set_value(i, std::move(r).value());
        } else {
          st->set_exception(std::move(r).error());
        }
      }
    } catch (...) {
      st->set_exception(std::current_exception());
    }
    st->arrive();
  });
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
  -> awaitable<std::tuple<::xz::io::detail::when_all_value_t<Ts>...>> {
  auto ex = co_await this_coro::executor;
  XZ_ENSURE(ex, "when_all: requires a bound executor");

  auto st = std::make_shared<::xz::io::detail::when_all_state<Ts...>>(ex);
  XZ_ENSURE(st->remaining.load(std::memory_order_relaxed) == sizeof...(Ts),
            "when_all: internal remaining init mismatch");
  detail::when_all_start_variadic<Ts...>(ex, st, std::tuple<awaitable<Ts>...>{std::move(tasks)...},
                                        std::index_sequence_for<Ts...>{});

  co_await ::xz::io::detail::await_when_all(st);

  std::exception_ptr ep{};
  typename ::xz::io::detail::when_all_state<Ts...>::values_tuple values{};
  {
    std::scoped_lock lk{st->m};
    ep = st->first_ep;
    values = std::move(st->values);
  }

  if (ep) std::rethrow_exception(ep);
  co_return detail::when_all_collect_variadic<Ts...>(std::move(values), std::index_sequence_for<Ts...>{});
}

/// Wait for all awaitables to complete (container).
///
/// Semantics are the same as the variadic overload.
template <class T>
auto when_all(std::vector<awaitable<T>> tasks)
  -> awaitable<std::conditional_t<std::is_void_v<T>, void, std::vector<std::remove_cvref_t<T>>>> {
  auto ex = co_await this_coro::executor;
  XZ_ENSURE(ex, "when_all(vector): requires a bound executor");

  auto st = std::make_shared<::xz::io::detail::when_all_container_state<T>>(ex, tasks.size());
  XZ_ENSURE(st->remaining.load(std::memory_order_relaxed) == tasks.size(),
            "when_all(vector): internal remaining init mismatch");

  for (std::size_t i = 0; i < tasks.size(); ++i) {
    detail::when_all_container_spawn_one<T>(ex, st, i, std::move(tasks[i]));
  }

  co_await ::xz::io::detail::await_when_all(st);

  std::exception_ptr ep{};
  std::vector<std::optional<::xz::io::detail::when_all_value_t<T>>> values{};
  {
    std::scoped_lock lk{st->m};
    ep = st->first_ep;
    if constexpr (!std::is_void_v<T>) {
      values = std::move(st->values);
    }
  }

  if (ep) std::rethrow_exception(ep);

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

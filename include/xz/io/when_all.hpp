#pragma once

#include <xz/io/awaitable.hpp>

#include <xz/io/assert.hpp>
#include <xz/io/co_spawn.hpp>
#include <xz/io/detail/when_all/state.hpp>
#include <xz/io/this_coro.hpp>
#include <xz/io/use_awaitable.hpp>

#include <cstddef>
#include <exception>
#include <optional>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace xz::io {

namespace detail {

template <std::size_t I, class... Ts>
auto when_all_await_one(std::tuple<awaitable<Ts>...>& waiters,
                        std::tuple<::xz::io::detail::when_all_value_t<Ts>...>& out,
                        std::exception_ptr& first_ep) -> awaitable<void> {
  using T = std::tuple_element_t<I, std::tuple<Ts...>>;
  try {
    if constexpr (std::is_void_v<T>) {
      co_await std::move(std::get<I>(waiters));
      std::get<I>(out) = std::monostate{};
    } else {
      std::get<I>(out) = co_await std::move(std::get<I>(waiters));
    }
  } catch (...) {
    if (!first_ep) {
      first_ep = std::current_exception();
    }
  }
}

template <std::size_t I, class... Ts>
auto when_all_await_all(std::tuple<awaitable<Ts>...>& waiters,
                        std::tuple<::xz::io::detail::when_all_value_t<Ts>...>& out,
                        std::exception_ptr& first_ep) -> awaitable<void> {
  if constexpr (I == sizeof...(Ts)) {
    co_return;
  } else {
    co_await when_all_await_one<I, Ts...>(waiters, out, first_ep);
    co_await when_all_await_all<I + 1, Ts...>(waiters, out, first_ep);
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
  -> awaitable<std::tuple<::xz::io::detail::when_all_value_t<Ts>...>> {
  auto ex = co_await this_coro::executor;
  XZ_ENSURE(ex, "when_all: requires a bound executor");

  // Hot-start all tasks on this executor.
  auto waiters = std::tuple<awaitable<Ts>...>{co_spawn(ex, std::move(tasks), use_awaitable)...};

  std::exception_ptr first_ep{};
  std::tuple<::xz::io::detail::when_all_value_t<Ts>...> out{};

  co_await detail::when_all_await_all<0, Ts...>(waiters, out, first_ep);

  if (first_ep) {
    std::rethrow_exception(first_ep);
  }

  co_return out;
}

/// Wait for all awaitables to complete (container).
///
/// Semantics are the same as the variadic overload.
template <class T>
auto when_all(std::vector<awaitable<T>> tasks)
  -> awaitable<std::conditional_t<std::is_void_v<T>, void, std::vector<std::remove_cvref_t<T>>>> {
  auto ex = co_await this_coro::executor;
  XZ_ENSURE(ex, "when_all(vector): requires a bound executor");

  std::exception_ptr first_ep{};
  std::vector<awaitable<T>> waiters;
  waiters.reserve(tasks.size());
  for (auto& t : tasks) {
    waiters.push_back(co_spawn(ex, std::move(t), use_awaitable));
  }

  if constexpr (std::is_void_v<T>) {
    for (auto& w : waiters) {
      try {
        co_await std::move(w);
      } catch (...) {
        if (!first_ep) {
          first_ep = std::current_exception();
        }
      }
    }
    if (first_ep) {
      std::rethrow_exception(first_ep);
    }
    co_return;
  } else {
    std::vector<std::remove_cvref_t<T>> out(tasks.size());
    for (std::size_t i = 0; i < waiters.size(); ++i) {
      try {
        out[i] = co_await std::move(waiters[i]);
      } catch (...) {
        if (!first_ep) {
          first_ep = std::current_exception();
        }
      }
    }
    if (first_ep) {
      std::rethrow_exception(first_ep);
    }
    co_return out;
  }
}

}  // namespace xz::io

#pragma once

#include <xz/io/awaitable.hpp>
#include <xz/io/detail/when_all_state.hpp>
#include <xz/io/detail/when_all_container_state.hpp>

#include <coroutine>
#include <memory>
#include <ranges>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace xz::io {

namespace detail {

template <typename... Ts>
struct when_all_awaiter {
  using result_type = std::tuple<std::conditional_t<std::is_void_v<Ts>, std::monostate, Ts>...>;

  std::shared_ptr<when_all_state<Ts...>> state_;

  explicit when_all_awaiter(std::shared_ptr<when_all_state<Ts...>> state)
      : state_(std::move(state)) {}

  auto await_ready() const noexcept -> bool {
    return false;
  }

  auto await_suspend(std::coroutine_handle<> h) -> bool {
    state_->continuation_ = h;
    // Children are started via io_context::post() (see start_awaitable), so no inline completion.
    state_->start_all(state_, std::make_index_sequence<sizeof...(Ts)>{});
    return true;
  }

  auto await_resume() -> result_type {
    return state_->get_result();
  }
};

template <typename T>
struct when_all_container_awaiter {
  using result_type = typename when_all_container_state<T>::result_type;

  std::shared_ptr<when_all_container_state<T>> state_;

  explicit when_all_container_awaiter(std::shared_ptr<when_all_container_state<T>> state)
      : state_(std::move(state)) {}

  auto await_ready() const noexcept -> bool {
    return state_->awaitables_.empty();
  }

  auto await_suspend(std::coroutine_handle<> h) -> bool {
    state_->continuation_ = h;
    state_->start_all(state_);
    return true;
  }

  auto await_resume() -> result_type {
    return state_->get_result();
  }
};

}  // namespace detail

/// Waits for all awaitables to complete and returns a tuple of their results.
/// If any awaitable throws an exception, when_all completes with that exception as soon as it is observed
/// (other awaitables may still run to completion; they are not cancelled).
/// For void awaitables, std::monostate is used in the result tuple.
///
/// Example:
///   auto [result1, result2] = co_await when_all(task1(), task2());
template <typename... Ts>
  requires (sizeof...(Ts) > 0)
auto when_all(awaitable<Ts>... awaitables) -> detail::when_all_awaiter<Ts...> {
  auto state = std::make_shared<detail::when_all_state<Ts...>>(std::move(awaitables)...);
  return detail::when_all_awaiter<Ts...>(std::move(state));
}

/// Waits for all awaitables in a container to complete and returns a vector of results.
/// If any awaitable throws an exception, when_all completes with that exception as soon as it is observed
/// (other awaitables may still run to completion; they are not cancelled).
/// For void awaitables, returns std::vector<std::monostate>.
///
/// Example:
///   std::vector<awaitable<int>> tasks;
///   tasks.push_back(task1());
///   tasks.push_back(task2());
///   auto results = co_await when_all(std::move(tasks));
template <typename T>
auto when_all(std::vector<awaitable<T>>&& awaitables) -> detail::when_all_container_awaiter<T> {
  auto state = std::make_shared<detail::when_all_container_state<T>>(std::move(awaitables));
  return detail::when_all_container_awaiter<T>(std::move(state));
}

}  // namespace xz::io

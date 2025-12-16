#pragma once

#include <xz/io/awaitable.hpp>
#include <xz/io/detail/when_any_state.hpp>
#include <xz/io/detail/when_any_container_state.hpp>

#include <coroutine>
#include <memory>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace xz::io {

namespace detail {

template <typename... Ts>
struct when_any_awaiter {
  using result_variant_t = std::variant<std::conditional_t<std::is_void_v<Ts>, std::monostate, Ts>...>;
  using result_type = std::pair<std::size_t, result_variant_t>;

  std::shared_ptr<when_any_state<Ts...>> state_;

  explicit when_any_awaiter(std::shared_ptr<when_any_state<Ts...>> state)
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
struct when_any_container_awaiter {
  using result_type = typename when_any_container_state<T>::result_type;

  std::shared_ptr<when_any_container_state<T>> state_;

  explicit when_any_container_awaiter(std::shared_ptr<when_any_container_state<T>> state)
      : state_(std::move(state)) {}

  auto await_ready() const noexcept -> bool {
    // Empty container should fail - require at least one task
    return false;
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

/// Waits for the first awaitable to complete and returns its index and result.
/// Returns std::pair<size_t, std::variant<Results...>> where the first element
/// is the index of the completed awaitable and the second is its result.
/// For void awaitables, std::monostate is used in the variant.
/// If the first completing awaitable throws, the exception is propagated.
///
/// Example:
///   auto [index, result] = co_await when_any(task1(), task2());
///   if (index == 0) {
///     auto value = std::get<0>(result);  // task1's result
///   }
template <typename... Ts>
  requires (sizeof...(Ts) > 0)
auto when_any(awaitable<Ts>... awaitables) -> detail::when_any_awaiter<Ts...> {
  auto state = std::make_shared<detail::when_any_state<Ts...>>(std::move(awaitables)...);
  return detail::when_any_awaiter<Ts...>(std::move(state));
}

/// Waits for the first awaitable in a container to complete and returns its index and result.
/// Returns std::pair<size_t, T> where the first element is the index of the completed
/// awaitable and the second is its result. For void awaitables, returns std::pair<size_t, std::monostate>.
/// If the first completing awaitable throws, the exception is propagated.
/// Requires at least one awaitable in the container.
///
/// Example:
///   std::vector<awaitable<int>> tasks;
///   tasks.push_back(task1());
///   tasks.push_back(task2());
///   auto [index, result] = co_await when_any(std::move(tasks));
template <typename T>
auto when_any(std::vector<awaitable<T>>&& awaitables) -> detail::when_any_container_awaiter<T> {
  auto state = std::make_shared<detail::when_any_container_state<T>>(std::move(awaitables));
  return detail::when_any_container_awaiter<T>(std::move(state));
}

}  // namespace xz::io

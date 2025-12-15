#pragma once

#include <xz/io/awaitable.hpp>
#include <xz/io/detail/when_all_state.hpp>

#include <coroutine>
#include <memory>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>

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
    // Start children first. If they all complete synchronously, do not suspend.
    state_->start_all(state_.get(), std::make_index_sequence<sizeof...(Ts)>{});
    if (state_->completed_) {
      return false;
    }
    state_->continuation_ = h;
    // Re-check to avoid missing completion if something finished right after we stored the continuation.
    if (state_->completed_) {
      state_->continuation_ = {};
      return false;
    }
    return true;
  }

  auto await_resume() -> result_type {
    return state_->get_result();
  }
};

}  // namespace detail

/// Waits for all awaitables to complete and returns a tuple of their results.
/// If any awaitable throws an exception, when_all immediately completes with that exception.
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

}  // namespace xz::io

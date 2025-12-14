#pragma once

#include <xz/io/awaitable.hpp>
#include <xz/io/io_context.hpp>

#include <concepts>
#include <exception>
#include <functional>
#include <memory>
#include <type_traits>
#include <utility>

namespace xz::io {

// Completion token tag types
struct detached_t {
  explicit detached_t() = default;
};

// Completion token instances
inline constexpr detached_t use_detached{};

namespace detail {

// Helper to check if a type is an awaitable
template <typename T>
struct is_awaitable : std::false_type {};

template <typename T>
struct is_awaitable<awaitable<T>> : std::true_type {};

template <typename T>
inline constexpr bool is_awaitable_v = is_awaitable<std::decay_t<T>>::value;

// Helper to extract awaitable result type
template <typename T>
struct awaitable_result;

template <typename T>
struct awaitable_result<awaitable<T>> {
  using type = T;
};

template <typename T>
using awaitable_result_t = typename awaitable_result<std::decay_t<T>>::type;

// State that holds the user awaitable and wrapper handle, keeping itself alive
template <typename T>
struct detached_state {
  awaitable<T> user_awaitable;
  std::coroutine_handle<> wrapper_handle;
  std::shared_ptr<detached_state> self;

  explicit detached_state(awaitable<T>&& t) : user_awaitable(std::move(t)) {}
};

// Wrapper coroutine that captures shared state to keep everything alive
template <typename T>
auto make_detached_wrapper(std::shared_ptr<detached_state<T>> state) -> awaitable<void> {
  try {
    if constexpr (std::is_void_v<T>) {
      co_await std::move(state->user_awaitable);
    } else {
      [[maybe_unused]] auto result = co_await std::move(state->user_awaitable);
    }
  } catch (...) {
    // Detached mode: swallow exceptions
  }
  // Clear self-reference to allow cleanup
  state->self.reset();
}

// Helper to spawn an awaitable on the executor
template <typename Executor, typename T>
void spawn_awaitable_detached(Executor& ex, awaitable<T>&& user_awaitable) {
  auto state = std::make_shared<detached_state<T>>(std::move(user_awaitable));

  // Create wrapper and transfer ownership of its coroutine handle
  auto wrapper = make_detached_wrapper(state);
  state->wrapper_handle = wrapper.release();
  state->self = state; // Create self-reference to keep alive

  ex.post([state]() mutable {
    state->wrapper_handle.resume();
  });
}

}  // namespace detail

// co_spawn with detached completion token
template <typename Executor, typename T>
  requires std::is_same_v<std::decay_t<Executor>, io_context>
void co_spawn(Executor& ex, awaitable<T>&& t, detached_t) {
  detail::spawn_awaitable_detached(ex, std::move(t));
}

// co_spawn with callable that returns an awaitable
template <typename Executor, typename F>
  requires std::is_same_v<std::decay_t<Executor>, io_context> &&
           std::invocable<F> &&
           detail::is_awaitable_v<std::invoke_result_t<F>>
void co_spawn(Executor& ex, F&& f, detached_t token) {
  auto t = std::forward<F>(f)();
  co_spawn(ex, std::move(t), token);
}

}  // namespace xz::io

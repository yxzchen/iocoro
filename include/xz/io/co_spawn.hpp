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

// State that holds the user awaitable and wrapper, keeping itself alive
template <typename T>
struct detached_state {
  std::optional<awaitable<T>> user_awaitable;
  std::optional<awaitable<void>> wrapper;
  std::shared_ptr<detached_state> self;
  std::optional<std::function<awaitable<T>()>> factory;  // Factory to create awaitable, keeps lambda alive

  explicit detached_state(awaitable<T>&& t) : user_awaitable(std::move(t)) {}
  explicit detached_state(std::function<awaitable<T>()>&& f) : factory(std::move(f)) {}
};

// Wrapper coroutine that captures shared state to keep everything alive
template <typename T>
auto make_detached_wrapper(std::shared_ptr<detached_state<T>> state) -> awaitable<void> {
  try {
    // Use factory if available, otherwise use user_awaitable directly
    if constexpr (std::is_void_v<T>) {
      if (state->factory.has_value()) {
        co_await std::move(state->factory.value())();
      } else {
        co_await std::move(*state->user_awaitable);
      }
    } else {
      [[maybe_unused]] auto result = state->factory.has_value()
        ? co_await std::move(state->factory.value())()
        : co_await std::move(*state->user_awaitable);
    }
  } catch (...) {
    // Detached mode: swallow exceptions
  }
  // Clear self-reference to allow cleanup
  state->self.reset();
}

// Helper to start an awaitable by resuming its coroutine handle
template <typename T>
void start_awaitable(awaitable<T>& a) {
  if (a.coro_) {
    a.coro_.resume();
  }
}

// Helper to spawn an awaitable on the executor
template <typename Executor, typename T>
void spawn_awaitable_detached(Executor& ex, awaitable<T>&& user_awaitable) {
  auto state = std::make_shared<detached_state<T>>(std::move(user_awaitable));
  state->wrapper.emplace(make_detached_wrapper(state));
  state->self = state; // Create self-reference to keep alive

  ex.post([state]() mutable {
    start_awaitable(*state->wrapper);
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
  // Use factory pattern to keep lambda alive
  using awaitable_t = std::invoke_result_t<F>;
  using result_t = detail::awaitable_result_t<awaitable_t>;
  auto state = std::make_shared<detail::detached_state<result_t>>(
    std::function<awaitable_t()>{std::forward<F>(f)}
  );
  state->wrapper.emplace(detail::make_detached_wrapper(state));
  state->self = state; // Create self-reference to keep alive

  ex.post([state]() mutable {
    detail::start_awaitable(*state->wrapper);
  });
}

}  // namespace xz::io

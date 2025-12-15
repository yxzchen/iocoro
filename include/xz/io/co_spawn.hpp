#pragma once

#include <xz/io/awaitable.hpp>
#include <xz/io/io_context.hpp>
#include <xz/io/detail/current_executor.hpp>

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

// State that holds the factory function and wrapper, keeping itself alive
template <typename T>
struct detached_state {
  std::function<awaitable<T>()> factory;  // Factory to create awaitable, keeps lambda alive
  std::optional<awaitable<void>> wrapper;
  std::shared_ptr<detached_state> self;

  explicit detached_state(std::function<awaitable<T>()>&& f) : factory(std::move(f)) {}
};

// Wrapper coroutine that captures shared state to keep everything alive
template <typename T>
auto make_detached_wrapper(std::shared_ptr<detached_state<T>> state) -> awaitable<void> {
  try {
    // Execute factory to create and run awaitable
    if constexpr (std::is_void_v<T>) {
      co_await state->factory();
    } else {
      [[maybe_unused]] auto result = co_await state->factory();
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
    // Enforce "no inline execution": schedule coroutine start on the event loop if available.
    detail::defer_start(a.coro_);
  }
}

}  // namespace detail

// co_spawn with detached completion token - direct awaitable overload
template <typename Executor, typename T>
  requires std::is_same_v<std::decay_t<Executor>, io_context>
void co_spawn(Executor& ex, awaitable<T>&& t, detached_t) {
  // Store awaitable in shared_ptr to make lambda copyable
  auto awaitable_ptr = std::make_shared<awaitable<T>>(std::move(t));
  auto state = std::make_shared<detail::detached_state<T>>(
    [awaitable_ptr]() mutable -> awaitable<T> {
      return std::move(*awaitable_ptr);
    }
  );
  state->wrapper.emplace(detail::make_detached_wrapper(state));
  state->self = state; // Create self-reference to keep alive

  ex.post([state]() mutable {
    detail::start_awaitable(*state->wrapper);
  });
}

// co_spawn with detached completion token - callable overload
template <typename Executor, typename F>
  requires std::is_same_v<std::decay_t<Executor>, io_context> &&
           (!detail::is_awaitable_v<std::decay_t<F>>)
void co_spawn(Executor& ex, F&& f, detached_t) {
  using result_t = detail::awaitable_result_t<std::invoke_result_t<F>>;
  auto state = std::make_shared<detail::detached_state<result_t>>(std::forward<F>(f));
  state->wrapper.emplace(detail::make_detached_wrapper(state));
  state->self = state; // Create self-reference to keep alive

  ex.post([state]() mutable {
    detail::start_awaitable(*state->wrapper);
  });
}

}  // namespace xz::io

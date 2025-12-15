#pragma once

#include <xz/io/awaitable.hpp>
#include <xz/io/io_context.hpp>

#include <concepts>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
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

template <typename T, typename Handler>
struct completion_state {
  std::function<awaitable<T>()> factory;  // Factory to create awaitable, keeps lambda alive
  Handler handler;
  std::optional<awaitable<void>> wrapper;
  std::shared_ptr<completion_state> self;

  completion_state(std::function<awaitable<T>()>&& f, Handler h)
      : factory(std::move(f)), handler(std::move(h)) {}
};

template <typename T, typename Handler>
auto make_completion_wrapper(std::shared_ptr<completion_state<T, Handler>> state) -> awaitable<void> {
  std::exception_ptr eptr;
  try {
    // Execute factory to create and run awaitable
    if constexpr (std::is_void_v<T>) {
      co_await state->factory();
    } else {
      [[maybe_unused]] auto result = co_await state->factory();
    }
  } catch (...) {
    eptr = std::current_exception();
  }

  try {
    std::invoke(state->handler, eptr);
  } catch (...) {
    // Completion handler should not throw across the event loop boundary.
  }

  // Clear self-reference to allow cleanup
  state->self.reset();
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

// co_spawn with completion handler void(std::exception_ptr) - direct awaitable overload
template <typename Executor, typename T, typename CompletionHandler>
  requires std::is_same_v<std::decay_t<Executor>, io_context> &&
           (!std::is_same_v<std::decay_t<CompletionHandler>, detached_t>) &&
           std::invocable<std::decay_t<CompletionHandler>&, std::exception_ptr>
void co_spawn(Executor& ex, awaitable<T>&& t, CompletionHandler&& handler) {
  using handler_t = std::decay_t<CompletionHandler>;
  // Store awaitable in shared_ptr to make lambda copyable
  auto awaitable_ptr = std::make_shared<awaitable<T>>(std::move(t));
  auto state = std::make_shared<detail::completion_state<T, handler_t>>(
    [awaitable_ptr]() mutable -> awaitable<T> {
      return std::move(*awaitable_ptr);
    },
    std::forward<CompletionHandler>(handler)
  );
  state->wrapper.emplace(detail::make_completion_wrapper(state));
  state->self = state; // Create self-reference to keep alive

  ex.post([state]() mutable {
    detail::start_awaitable(*state->wrapper);
  });
}

// co_spawn with completion handler void(std::exception_ptr) - callable overload
template <typename Executor, typename F, typename CompletionHandler>
  requires std::is_same_v<std::decay_t<Executor>, io_context> &&
           (!detail::is_awaitable_v<std::decay_t<F>>) &&
           (!std::is_same_v<std::decay_t<CompletionHandler>, detached_t>) &&
           std::invocable<std::decay_t<CompletionHandler>&, std::exception_ptr>
void co_spawn(Executor& ex, F&& f, CompletionHandler&& handler) {
  using result_t = detail::awaitable_result_t<std::invoke_result_t<F>>;
  using handler_t = std::decay_t<CompletionHandler>;

  auto state = std::make_shared<detail::completion_state<result_t, handler_t>>(
    std::forward<F>(f),
    std::forward<CompletionHandler>(handler)
  );
  state->wrapper.emplace(detail::make_completion_wrapper(state));
  state->self = state; // Create self-reference to keep alive

  ex.post([state]() mutable {
    detail::start_awaitable(*state->wrapper);
  });
}

}  // namespace xz::io

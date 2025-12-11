#pragma once

#include <xz/io/io_context.hpp>
#include <xz/io/task.hpp>

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

// Helper to check if a type is a task
template <typename T>
struct is_task : std::false_type {};

template <typename T>
struct is_task<task<T>> : std::true_type {};

template <typename T>
inline constexpr bool is_task_v = is_task<std::decay_t<T>>::value;

// Helper to extract task result type
template <typename T>
struct task_result;

template <typename T>
struct task_result<task<T>> {
  using type = T;
};

template <typename T>
using task_result_t = typename task_result<std::decay_t<T>>::type;

// State that holds the user task and wrapper, keeping itself alive
template <typename T>
struct detached_state {
  task<T> user_task;
  std::optional<task<void>> wrapper;
  std::shared_ptr<detached_state> self;

  explicit detached_state(task<T>&& t) : user_task(std::move(t)) {}
};

// Wrapper coroutine that captures shared state to keep everything alive
template <typename T>
auto make_detached_wrapper(std::shared_ptr<detached_state<T>> state) -> task<void> {
  try {
    if constexpr (std::is_void_v<T>) {
      co_await std::move(state->user_task);
    } else {
      [[maybe_unused]] auto result = co_await std::move(state->user_task);
    }
  } catch (...) {
    // Detached mode: swallow exceptions
  }
  // Clear self-reference to allow cleanup
  state->self.reset();
}

// Helper to spawn a task on the executor
template <typename Executor, typename T>
void spawn_task_detached(Executor& ex, task<T>&& user_task) {
  auto state = std::make_shared<detached_state<T>>(std::move(user_task));
  state->wrapper.emplace(make_detached_wrapper(state));
  state->self = state; // Create self-reference to keep alive

  ex.post([state]() mutable {
    state->wrapper->resume();
  });
}

}  // namespace detail

// co_spawn with detached completion token
template <typename Executor, typename T>
  requires std::is_same_v<std::decay_t<Executor>, io_context>
void co_spawn(Executor& ex, task<T>&& t, detached_t) {
  detail::spawn_task_detached(ex, std::move(t));
}

// co_spawn with callable that returns a task
template <typename Executor, typename F>
  requires std::is_same_v<std::decay_t<Executor>, io_context> &&
           std::invocable<F> &&
           detail::is_task_v<std::invoke_result_t<F>>
void co_spawn(Executor& ex, F&& f, detached_t token) {
  auto t = std::forward<F>(f)();
  co_spawn(ex, std::move(t), token);
}

}  // namespace xz::io

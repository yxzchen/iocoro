#pragma once

#include <xz/io/error.hpp>
#include <xz/io/io_context.hpp>
#include <xz/io/detail/current_executor.hpp>
#include <xz/io/this_coro.hpp>

#include <coroutine>
#include <functional>
#include <memory>
#include <optional>
#include <system_error>
#include <utility>
#include <variant>

namespace xz::io {

// Forward declaration
template <typename T>
class awaitable;

namespace detail {
// Forward declarations for friend
template <typename T>
struct detached_state;

template <typename T>
void start_awaitable(awaitable<T>& a);

template <typename Executor, typename T>
void spawn_awaitable_detached(Executor& ex, awaitable<T>&& user_awaitable);
}  // namespace detail

/// Coroutine type for async operations
template <typename T = void>
class awaitable;

namespace detail {
/// Base promise type with common functionality
template <typename Derived>
struct awaitable_promise_base {
  std::exception_ptr exception_;
  std::coroutine_handle<> continuation_;

  std::suspend_always initial_suspend() noexcept { return {}; }

  auto final_suspend() noexcept {
    struct final_awaiter {
      bool await_ready() noexcept { return false; }
      auto await_suspend(std::coroutine_handle<Derived> h) noexcept -> std::coroutine_handle<> {
        if (h.promise().continuation_) {
          // Enforce "no inline resumption": schedule continuation on the event loop.
          detail::defer_resume(h.promise().continuation_);
        }
        return std::noop_coroutine();
      }
      void await_resume() noexcept {}
    };
    return final_awaiter{};
  }

  void unhandled_exception() { exception_ = std::current_exception(); }

  // Allow `co_await this_coro::executor` to access the current io_context.
  auto await_transform(this_coro::executor_t) noexcept -> detail::this_executor_awaiter {
    return {};
  }

  // Pass-through for all other awaitables/awaiters.
  template <typename U>
  auto await_transform(U&& u) noexcept -> decltype(auto) {
    return std::forward<U>(u);
  }
};

/// Promise type for non-void awaitables
template <typename T>
struct awaitable_promise : awaitable_promise_base<awaitable_promise<T>> {
  std::optional<T> value_;

  auto get_return_object() -> awaitable<T>;

  template <typename U>
    requires std::convertible_to<U, T>
  void return_value(U&& value) {
    value_.emplace(std::forward<U>(value));
  }
};

/// Promise type for void awaitables
template <>
struct awaitable_promise<void> : awaitable_promise_base<awaitable_promise<void>> {
  auto get_return_object() -> awaitable<void>;

  void return_void() noexcept {}
};
}  // namespace detail

/// A coroutine type that represents an asynchronous operation
template <typename T>
class awaitable {
 public:
  using promise_type = detail::awaitable_promise<T>;
  using handle_type = std::coroutine_handle<promise_type>;

  explicit awaitable(handle_type h) : coro_(h) {}

  awaitable(awaitable&& other) noexcept : coro_(std::exchange(other.coro_, {})) {}

  auto operator=(awaitable&& other) noexcept -> awaitable& {
    if (this != &other) {
      if (coro_) coro_.destroy();
      coro_ = std::exchange(other.coro_, {});
    }
    return *this;
  }

  ~awaitable() {
    if (coro_) coro_.destroy();
  }

  awaitable(awaitable const&) = delete;
  auto operator=(awaitable const&) -> awaitable& = delete;

  auto await_ready() const noexcept -> bool { return false; }

  auto await_suspend(std::coroutine_handle<> awaiting) noexcept -> std::coroutine_handle<> {
    coro_.promise().continuation_ = awaiting;
    return coro_;
  }

  auto await_resume() {
    if (coro_.promise().exception_) {
      std::rethrow_exception(coro_.promise().exception_);
    }
    if constexpr (!std::is_void_v<T>) {
      return std::move(*coro_.promise().value_);
    }
  }

 private:
  handle_type coro_;

  template <typename U>
  friend struct detail::detached_state;

  template <typename U>
  friend void detail::start_awaitable(awaitable<U>& a);

  template <typename Executor, typename U>
  friend void detail::spawn_awaitable_detached(Executor& ex, awaitable<U>&& user_awaitable);
};

namespace detail {
template <typename T>
auto awaitable_promise<T>::get_return_object() -> awaitable<T> {
  return awaitable<T>{std::coroutine_handle<awaitable_promise<T>>::from_promise(*this)};
}

inline auto awaitable_promise<void>::get_return_object() -> awaitable<void> {
  return awaitable<void>{std::coroutine_handle<awaitable_promise<void>>::from_promise(*this)};
}

// Helper to start an awaitable by scheduling its coroutine handle on the event loop.
// Defined here so users of when_any/when_all don't need to include co_spawn.hpp.
template <typename T>
inline void start_awaitable(awaitable<T>& a) {
  if (a.coro_) {
    defer_start(a.coro_);
  }
}
}  // namespace detail

}  // namespace xz::io

#pragma once

#include <coroutine>
#include <exception>
#include <type_traits>
#include <utility>
#include <variant>

namespace xz::io {

namespace detail {
/// Base promise type with common functionality
template <typename Derived>
struct task_promise_base {
  std::exception_ptr exception_;
  std::coroutine_handle<> continuation_;

  std::suspend_always initial_suspend() noexcept { return {}; }

  auto final_suspend() noexcept {
    struct final_awaiter {
      bool await_ready() noexcept { return false; }
      auto await_suspend(std::coroutine_handle<Derived> h) noexcept -> std::coroutine_handle<> {
        if (h.promise().continuation_) {
          return h.promise().continuation_;
        }
        return std::noop_coroutine();
      }
      void await_resume() noexcept {}
    };
    return final_awaiter{};
  }

  void unhandled_exception() { exception_ = std::current_exception(); }
};

/// Promise type for non-void tasks
template <typename T>
struct task_promise : task_promise_base<task_promise<T>> {
  T value_;

  auto get_return_object() -> auto;

  template <typename U>
    requires std::convertible_to<U, T>
  void return_value(U&& value) {
    value_ = std::forward<U>(value);
  }
};

/// Promise type for void tasks
template <>
struct task_promise<void> : task_promise_base<task_promise<void>> {
  auto get_return_object() -> auto;

  void return_void() noexcept {}
};
}  // namespace detail

/// A coroutine task type that represents an asynchronous operation
template <typename T = void>
class task {
 public:
  using promise_type = detail::task_promise<T>;
  using handle_type = std::coroutine_handle<promise_type>;

  explicit task(handle_type h) : coro_(h) {}

  task(task&& other) noexcept : coro_(std::exchange(other.coro_, {})) {}

  auto operator=(task&& other) noexcept -> task& {
    if (this != &other) {
      if (coro_) coro_.destroy();
      coro_ = std::exchange(other.coro_, {});
    }
    return *this;
  }

  ~task() {
    if (coro_) coro_.destroy();
  }

  task(task const&) = delete;
  auto operator=(task const&) -> task& = delete;

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
      return std::move(coro_.promise().value_);
    }
  }

  auto resume() -> bool {
    if (!coro_ || coro_.done()) return false;
    coro_.resume();
    return !coro_.done();
  }

 private:
  handle_type coro_;
};

namespace detail {
template <typename T>
auto task_promise<T>::get_return_object() -> auto {
  return task<T>{std::coroutine_handle<task_promise<T>>::from_promise(*this)};
}

inline auto task_promise<void>::get_return_object() -> auto {
  return task<void>{std::coroutine_handle<task_promise<void>>::from_promise(*this)};
}
}  // namespace detail

}  // namespace xz::io

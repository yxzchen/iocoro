#pragma once

#include <coroutine>
#include <exception>
#include <type_traits>
#include <utility>

namespace xz::io {

/// A coroutine task type that represents an asynchronous operation
template <typename T = void>
class task {
 public:
  struct promise_type {
    std::exception_ptr exception_;
    T value_;

    task get_return_object() { return task{std::coroutine_handle<promise_type>::from_promise(*this)}; }

    std::suspend_always initial_suspend() noexcept { return {}; }
    std::suspend_always final_suspend() noexcept { return {}; }

    void unhandled_exception() { exception_ = std::current_exception(); }

    template <typename U>
      requires std::convertible_to<U, T>
    void return_value(U&& value) {
      value_ = std::forward<U>(value);
    }
  };

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
    return coro_;
  }

  auto await_resume() -> T {
    if (coro_.promise().exception_) {
      std::rethrow_exception(coro_.promise().exception_);
    }
    return std::move(coro_.promise().value_);
  }

  auto resume() -> bool {
    if (!coro_ || coro_.done()) return false;
    coro_.resume();
    return !coro_.done();
  }

 private:
  handle_type coro_;
};

/// Specialization for void return type
template <>
class task<void> {
 public:
  struct promise_type {
    std::exception_ptr exception_;
    std::coroutine_handle<> continuation_;

    task get_return_object() { return task{std::coroutine_handle<promise_type>::from_promise(*this)}; }

    std::suspend_always initial_suspend() noexcept { return {}; }

    auto final_suspend() noexcept {
      struct final_awaiter {
        bool await_ready() noexcept { return false; }
        auto await_suspend(std::coroutine_handle<promise_type> h) noexcept -> std::coroutine_handle<> {
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

    void return_void() noexcept {}
  };

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

  void await_resume() {
    if (coro_.promise().exception_) {
      std::rethrow_exception(coro_.promise().exception_);
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

}  // namespace xz::io

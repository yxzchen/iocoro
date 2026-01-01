#pragma once

#include <iocoro/detail/awaitable_promise.hpp>

#include <coroutine>
#include <exception>
#include <utility>

namespace iocoro {

template <typename T>
class awaitable {
 public:
  using promise_type = detail::awaitable_promise<T>;
  using handle_type = std::coroutine_handle<promise_type>;

  explicit awaitable(handle_type h) noexcept : coro_(h) {}
  ~awaitable() {
    if (coro_) {
      coro_.destroy();
    }
  }

  awaitable(awaitable const&) = delete;
  auto operator=(awaitable const&) -> awaitable& = delete;

  awaitable(awaitable&& other) noexcept : coro_(std::exchange(other.coro_, {})) {}
  auto operator=(awaitable&& other) noexcept -> awaitable& {
    if (this != &other) {
      if (coro_) {
        coro_.destroy();
      }
      coro_ = std::exchange(other.coro_, {});
    }
    return *this;
  }

  /// Release ownership of the coroutine handle without destroying it.
  ///
  /// This is primarily used by spawn utilities (e.g. co_spawn) to take over the
  /// coroutine frame lifetime.
  auto release() noexcept -> handle_type { return std::exchange(coro_, {}); }

  auto get_executor() const noexcept -> any_executor {
    if (!coro_) {
      return any_executor{};
    }
    return coro_.promise().get_executor();
  }

  bool await_ready() const noexcept { return false; }
  auto await_suspend(std::coroutine_handle<> h) noexcept -> std::coroutine_handle<> {
    coro_.promise().set_continuation(h);
    return coro_;
  }
  auto await_resume() -> T {
    coro_.promise().rethrow_if_exception();
    return coro_.promise().take_value();
  }

 private:
  handle_type coro_;
};

template <>
class awaitable<void> {
 public:
  using promise_type = detail::awaitable_promise<void>;
  using handle_type = std::coroutine_handle<promise_type>;

  explicit awaitable(handle_type h) noexcept : coro_(h) {}
  ~awaitable() {
    if (coro_) {
      coro_.destroy();
    }
  }

  awaitable(awaitable const&) = delete;
  auto operator=(awaitable const&) -> awaitable& = delete;

  awaitable(awaitable&& other) noexcept : coro_(std::exchange(other.coro_, {})) {}
  auto operator=(awaitable&& other) noexcept -> awaitable& {
    if (this != &other) {
      if (coro_) {
        coro_.destroy();
      }
      coro_ = std::exchange(other.coro_, {});
    }
    return *this;
  }

  /// Release ownership of the coroutine handle without destroying it.
  auto release() noexcept -> handle_type { return std::exchange(coro_, {}); }

  auto get_executor() const noexcept -> any_executor {
    if (!coro_) {
      return any_executor{};
    }
    return coro_.promise().get_executor();
  }

  bool await_ready() const noexcept { return false; }
  auto await_suspend(std::coroutine_handle<> h) noexcept -> std::coroutine_handle<> {
    coro_.promise().set_continuation(h);
    return coro_;
  }
  void await_resume() {
    coro_.promise().rethrow_if_exception();
    coro_.promise().take_value();
  }

 private:
  handle_type coro_;
};

}  // namespace iocoro

namespace iocoro::detail {

template <typename T>
auto awaitable_promise<T>::get_return_object() -> awaitable<T> {
  using promise_t = awaitable_promise<T>;
  return awaitable<T>{std::coroutine_handle<promise_t>::from_promise(*this)};
}

inline auto awaitable_promise<void>::get_return_object() -> awaitable<void> {
  using promise_t = awaitable_promise<void>;
  return awaitable<void>{std::coroutine_handle<promise_t>::from_promise(*this)};
}

}  // namespace iocoro::detail

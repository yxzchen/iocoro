#pragma once

#include <xz/io/detail/awaitable/awaitable_promise.hpp>

#include <coroutine>
#include <exception>
#include <utility>

namespace xz::io {

template <typename T>
class awaitable {
 public:
  using promise_type = detail::awaitable_promise<T>;
  using handle_type = std::coroutine_handle<promise_type>;

  explicit awaitable(handle_type h) noexcept : coro_(h) {}
  ~awaitable() {
    if (coro_) coro_.destroy();
  }

  awaitable(awaitable const&) = delete;
  auto operator=(awaitable const&) -> awaitable& = delete;

  awaitable(awaitable&& other) noexcept : coro_(std::exchange(other.coro_, {})) {}
  auto operator=(awaitable&& other) noexcept -> awaitable& {
    if (this != &other) {
      if (coro_) coro_.destroy();
      coro_ = std::exchange(other.coro_, {});
    }
    return *this;
  }

  bool await_ready() const noexcept { return false; }
  auto await_suspend(std::coroutine_handle<> cont) noexcept -> std::coroutine_handle<> {
    coro_.promise().set_continuation(cont);
    return coro_;
  }
  auto await_resume() -> T {
    coro_.promise().rethrow_if_exception();
    return coro_.promise().take_value();
  }

 private:
  template <typename U>
  friend void co_spawn(class executor ex, awaitable<U> a);

  handle_type coro_;
};

template <>
class awaitable<void> {
 public:
  using promise_type = detail::awaitable_promise<void>;
  using handle_type = std::coroutine_handle<promise_type>;

  explicit awaitable(handle_type h) noexcept : coro_(h) {}

  awaitable(awaitable&& other) noexcept : coro_(std::exchange(other.coro_, {})) {}
  ~awaitable() {
    if (coro_) coro_.destroy();
  }

  awaitable(awaitable const&) = delete;
  auto operator=(awaitable const&) -> awaitable& = delete;

  bool await_ready() const noexcept { return false; }
  auto await_suspend(std::coroutine_handle<> cont) noexcept -> std::coroutine_handle<> {
    coro_.promise().set_continuation(cont);
    return coro_;
  }
  void await_resume() {
    coro_.promise().rethrow_if_exception();
    coro_.promise().take_value();
  }

 private:
  template <typename U>
  friend void co_spawn(class executor ex, awaitable<U> a);

  handle_type coro_;
};

}  // namespace xz::io

namespace xz::io::detail {

template <typename T>
auto awaitable_promise<T>::get_return_object() -> ::xz::io::awaitable<T> {
  using promise_t = awaitable_promise<T>;
  return ::xz::io::awaitable<T>{std::coroutine_handle<promise_t>::from_promise(*this)};
}

inline auto awaitable_promise<void>::get_return_object() -> ::xz::io::awaitable<void> {
  using promise_t = awaitable_promise<void>;
  return ::xz::io::awaitable<void>{std::coroutine_handle<promise_t>::from_promise(*this)};
}

}  // namespace xz::io::detail

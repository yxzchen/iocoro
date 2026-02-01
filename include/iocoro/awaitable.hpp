#pragma once

#include <iocoro/detail/awaitable_promise.hpp>

#include <coroutine>
#include <exception>
#include <memory>
#include <stop_token>
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

  auto get_stop_token() -> std::stop_token {
    if (!coro_) {
      return {};
    }
    return coro_.promise().get_stop_token();
  }

  void request_stop() noexcept {
    if (coro_) {
      coro_.promise().request_stop();
    }
  }

  bool await_ready() const noexcept { return false; }
  template <class Promise>
  auto await_suspend(std::coroutine_handle<Promise> h) noexcept -> std::coroutine_handle<> {
    coro_.promise().set_continuation(h);
    if constexpr (requires { h.promise().get_executor(); }) {
      coro_.promise().inherit_executor(h.promise().get_executor());
    }
    if constexpr (requires { h.promise().get_stop_token(); }) {
      coro_.promise().inherit_stop_token(h.promise().get_stop_token());
    }
    return coro_;
  }
  auto await_resume() -> T {
    coro_.promise().rethrow_if_exception();
    return coro_.promise().take_value();
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

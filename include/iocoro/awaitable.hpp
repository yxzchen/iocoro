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

  /// Owning handle wrapper for an `awaitable_promise<T>` coroutine.
  ///
  /// IMPORTANT: This type owns the coroutine frame. If it still owns a handle on destruction,
  /// it will `destroy()` the coroutine.
  explicit awaitable(handle_type h) noexcept : coro_(h) {}
  ~awaitable() noexcept {
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
  /// SAFETY: After `release()`, the caller becomes responsible for eventually destroying the
  /// handle (or transferring ownership elsewhere).
  [[nodiscard]] auto release() noexcept -> handle_type { return std::exchange(coro_, {}); }

  /// Return the executor associated with this coroutine (if any).
  [[nodiscard]] auto get_executor() const noexcept -> any_executor {
    if (!coro_) {
      return any_executor{};
    }
    return coro_.promise().get_executor();
  }

  /// Return the stop token associated with this coroutine (if any).
  [[nodiscard]] auto get_stop_token() const noexcept -> std::stop_token {
    if (!coro_) {
      return {};
    }
    return coro_.promise().get_stop_token();
  }

  /// Request stop for this coroutine (if supported by the promise).
  void request_stop() noexcept {
    if (coro_) {
      coro_.promise().request_stop();
    }
  }

  bool await_ready() const noexcept { return false; }
  template <class Promise>
  auto await_suspend(std::coroutine_handle<Promise> h) -> std::coroutine_handle<> {
    // NOTE: We always suspend and resume through `coro_`. The continuation is stored in the
    // awaited coroutine's promise.
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

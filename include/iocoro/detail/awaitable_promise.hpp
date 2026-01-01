#pragma once

#include <iocoro/assert.hpp>
#include <iocoro/executor.hpp>
#include <iocoro/io_executor.hpp>
#include <iocoro/this_coro.hpp>

#include <cassert>
#include <coroutine>
#include <exception>
#include <optional>
#include <utility>

namespace iocoro {
template <typename T>
class awaitable;
}  // namespace iocoro

namespace iocoro::detail {

struct awaitable_promise_base {
  any_executor ex_{};
  std::coroutine_handle<> continuation_{};
  std::exception_ptr exception_{};
  bool detached_{false};

  awaitable_promise_base() noexcept = default;

  std::suspend_always initial_suspend() noexcept { return {}; }

  auto final_suspend() noexcept {
    struct final_awaiter {
      awaitable_promise_base* self;

      bool await_ready() noexcept { return false; }

      void await_suspend(std::coroutine_handle<> h) noexcept {
        // If detached, the coroutine owns its own lifetime.
        if (self->detached_) {
          // Detached coroutines must not have a continuation.
          self->ex_.post([h, ex = self->ex_]() mutable { h.destroy(); });
          return;
        }

        self->resume_continuation();
      }

      void await_resume() noexcept {}
    };

    return final_awaiter{this};
  }

  void set_executor(any_executor ex) noexcept { ex_ = std::move(ex); }

  void detach() noexcept {
    IOCORO_ENSURE(ex_, "awaitable_promise: detach() requires executor");
    detached_ = true;
  }

  void set_continuation(std::coroutine_handle<> h) noexcept {
    continuation_ = h;
    // Child coroutines inherit the current executor by default.
    if (!ex_) {
      ex_ = detail::get_current_executor();
    }
  }

  void resume_continuation() noexcept {
    if (!continuation_) {
      return;
    }

    IOCORO_ENSURE(ex_, "awaitable_promise: resume_continuation() requires executor");

    ex_.post([h = continuation_]() { h.resume(); });
  }

  void unhandled_exception() noexcept { exception_ = std::current_exception(); }

  void rethrow_if_exception() {
    if (exception_) {
      std::rethrow_exception(exception_);
    }
  }

  template <typename Awaitable>
  decltype(auto) await_transform(Awaitable&& a) noexcept {
    return std::forward<Awaitable>(a);
  }

  auto await_transform(this_coro::executor_t) noexcept {
    struct awaiter {
      any_executor ex;
      bool await_ready() noexcept { return true; }
      any_executor await_resume() noexcept { return ex; }
      void await_suspend(std::coroutine_handle<>) noexcept {}
    };
    return awaiter{ex_};
  }
};

template <typename T>
struct awaitable_promise final : awaitable_promise_base {
  std::optional<T> value_{};

  awaitable_promise() noexcept = default;

  auto get_return_object() -> awaitable<T>;

  template <typename U>
    requires std::convertible_to<U, T>
  void return_value(U&& v) {
    value_.emplace(std::forward<U>(v));
  }

  auto take_value() -> T {
    assert(value_.has_value());
    return std::move(*value_);
  }
};

template <>
struct awaitable_promise<void> final : awaitable_promise_base {
  awaitable_promise() noexcept = default;

  auto get_return_object() -> awaitable<void>;
  void return_void() noexcept {}

  void take_value() noexcept {}
};

}  // namespace iocoro::detail

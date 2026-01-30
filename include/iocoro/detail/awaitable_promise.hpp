#pragma once

#include <iocoro/any_executor.hpp>
#include <iocoro/any_io_executor.hpp>
#include <iocoro/assert.hpp>
#include <iocoro/detail/executor_cast.hpp>
#include <stop_token>
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
  std::stop_token tok_{};
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
          self->ex_.post([h]() mutable { h.destroy(); });
          return;
        }

        self->resume_continuation();
      }

      void await_resume() noexcept {}
    };

    return final_awaiter{this};
  }

  auto get_executor() noexcept { return ex_; }
  void set_executor(any_executor ex) noexcept { ex_ = std::move(ex); }

  auto get_stop_token() const noexcept -> std::stop_token { return tok_; }
  void set_stop_token(std::stop_token tok) noexcept { tok_ = std::move(tok); }

  void inherit_executor(any_executor parent_ex) noexcept {
    if (!ex_) {
      ex_ = std::move(parent_ex);
    }
  }

  void inherit_stop_token(std::stop_token parent_tok) noexcept {
    if (!tok_.stop_possible()) {
      tok_ = std::move(parent_tok);
    }
  }

  void detach() noexcept {
    IOCORO_ENSURE(ex_, "awaitable_promise: detach() requires executor");
    detached_ = true;
  }

  void set_continuation(std::coroutine_handle<> h) noexcept {
    continuation_ = h;
  }

  void resume_continuation() noexcept {
    if (!continuation_) {
      return;
    }
    post_resume(continuation_);
  }

  void post_resume(std::coroutine_handle<> h) noexcept {
    IOCORO_ENSURE(ex_, "awaitable_promise: post_resume() requires executor");
    ex_.post([h]() mutable { h.resume(); });
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

  auto await_transform(this_coro::io_executor_t) noexcept {
    struct awaiter {
      any_executor ex;
      bool await_ready() noexcept { return true; }
      auto await_resume() noexcept -> any_io_executor {
        return require_io_executor(ex);
      }
      void await_suspend(std::coroutine_handle<>) noexcept {}
    };
    return awaiter{ex_};
  }

  auto await_transform(this_coro::stop_token_t) noexcept {
    struct awaiter {
      std::stop_token tok;
      bool await_ready() noexcept { return true; }
      auto await_resume() noexcept -> std::stop_token { return tok; }
      void await_suspend(std::coroutine_handle<>) noexcept {}
    };
    return awaiter{tok_};
  }

  struct stop_scope {
    awaitable_promise_base* self{};
    std::stop_token prev{};

    stop_scope() noexcept = default;
    stop_scope(awaitable_promise_base* s, std::stop_token p) noexcept
        : self(s), prev(std::move(p)) {}

    stop_scope(stop_scope const&) = delete;
    auto operator=(stop_scope const&) -> stop_scope& = delete;

    stop_scope(stop_scope&& other) noexcept
        : self(std::exchange(other.self, nullptr)), prev(std::move(other.prev)) {}

    auto operator=(stop_scope&& other) noexcept -> stop_scope& {
      if (this != &other) {
        reset();
        self = std::exchange(other.self, nullptr);
        prev = std::move(other.prev);
      }
      return *this;
    }

    ~stop_scope() { reset(); }

    void reset() noexcept {
      if (self) {
        self->set_stop_token(std::move(prev));
        self = nullptr;
      }
    }
  };

  auto await_transform(this_coro::set_stop_token_t t) noexcept {
    struct awaiter {
      awaitable_promise_base* self;
      std::stop_token next;

      bool await_ready() noexcept { return true; }
      auto await_resume() noexcept -> stop_scope {
        auto prev = self->get_stop_token();
        self->set_stop_token(std::move(next));
        return stop_scope{self, std::move(prev)};
      }
      void await_suspend(std::coroutine_handle<>) noexcept {}
    };
    return awaiter{this, std::move(t.tok)};
  }

  template <class Rep, class Period>
  auto await_transform(this_coro::scoped_timeout_t<Rep, Period> t) noexcept;

  auto await_transform(this_coro::switch_to_t t) noexcept {
    struct awaiter {
      awaitable_promise_base* self;
      any_executor target;

      bool await_ready() noexcept {
        // switch_to never short-circuits based on "current == target".
        // The adapter does not inspect TLS or compare executors; it always schedules.
        return false;
      }

      auto await_suspend(std::coroutine_handle<> h) noexcept -> bool {
        IOCORO_ENSURE(target, "this_coro::switch_to: empty executor");

        self->ex_ = std::move(target);
        self->post_resume(h);
        return true;
      }

      void await_resume() noexcept {}
    };

    return awaiter{this, std::move(t.ex)};
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

// scoped_timeout is implemented via promise::await_transform, but we keep all timeout
// state private and out of the promise itself.
#include <iocoro/timeout.hpp>

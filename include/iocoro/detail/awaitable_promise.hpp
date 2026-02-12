#pragma once

#include <iocoro/any_executor.hpp>
#include <iocoro/any_io_executor.hpp>
#include <iocoro/assert.hpp>
#include <iocoro/detail/executor_cast.hpp>
#include <iocoro/detail/executor_guard.hpp>
#include <iocoro/detail/unique_function.hpp>
#include <iocoro/this_coro.hpp>

#include <cassert>
#include <coroutine>
#include <exception>
#include <memory>
#include <optional>
#include <stop_token>
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
  std::stop_source stop_source_{};
  std::unique_ptr<std::stop_callback<unique_function<void()>>> parent_stop_cb_{};

  awaitable_promise_base() noexcept = default;

  std::suspend_always initial_suspend() noexcept { return {}; }

  auto final_suspend() noexcept {
    struct final_awaiter {
      awaitable_promise_base* self;

      bool await_ready() noexcept { return false; }

      auto await_suspend(std::coroutine_handle<> h) noexcept -> std::coroutine_handle<> {
        self->parent_stop_cb_.reset();
        // If detached, the coroutine owns its own lifetime.
        if (self->detached_) {
          // Detached coroutines must not have a continuation.
          h.destroy();
          return std::noop_coroutine();
        }

        auto cont = std::exchange(self->continuation_, std::coroutine_handle<>{});
        if (!cont) {
          return std::noop_coroutine();
        }

        if (detail::get_current_executor() == self->ex_) {
          return cont;
        }

        auto ex = self->ex_;
        ex.post([cont]() mutable { cont.resume(); });
        return std::noop_coroutine();
      }

      void await_resume() noexcept {}
    };

    return final_awaiter{this};
  }

  auto get_executor() const noexcept { return ex_; }
  void set_executor(any_executor ex) noexcept { ex_ = std::move(ex); }

  void inherit_executor(any_executor parent_ex) noexcept {
    if (!ex_) {
      ex_ = std::move(parent_ex);
    }
  }

  auto get_stop_token() const noexcept -> std::stop_token { return stop_source_.get_token(); }

  void inherit_stop_token(std::stop_token parent) {
    if (!parent.stop_possible() || parent_stop_cb_) {
      return;
    }
    if (parent.stop_requested()) {
      request_stop();
      return;
    }
    parent_stop_cb_ = std::make_unique<std::stop_callback<unique_function<void()>>>(
      parent, unique_function<void()>{[this]() { request_stop(); }});
  }

  void request_stop() noexcept { stop_source_.request_stop(); }

  auto stop_requested() const noexcept -> bool { return stop_source_.stop_requested(); }

  void detach() noexcept {
    IOCORO_ENSURE(ex_, "awaitable_promise: detach() requires executor");
    detached_ = true;
  }

  void set_continuation(std::coroutine_handle<> h) noexcept { continuation_ = h; }

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
      auto await_resume() noexcept -> any_io_executor { return to_io_executor(ex); }
      void await_suspend(std::coroutine_handle<>) noexcept {}
    };
    return awaiter{ex_};
  }

  auto await_transform(this_coro::stop_token_t) noexcept {
    struct awaiter {
      std::stop_token token;
      bool await_ready() noexcept { return true; }
      auto await_resume() noexcept -> std::stop_token { return token; }
      void await_suspend(std::coroutine_handle<>) noexcept {}
    };
    return awaiter{get_stop_token()};
  }

  auto await_transform(this_coro::switch_to_t t) noexcept {
    struct awaiter {
      awaitable_promise_base* self;
      any_executor target;

      bool await_ready() noexcept {
        IOCORO_ENSURE(target, "this_coro::switch_to: empty executor");

        // Fast-path: if we're already running under the target executor, continue inline.
        // This avoids an unnecessary post() and keeps switch_to cheap for same-executor hops.
        if (detail::get_current_executor() == target) {
          self->ex_ = target;
          return true;
        }
        return false;
      }

      auto await_suspend(std::coroutine_handle<> h) noexcept -> bool {
        self->ex_ = target;
        target.post([h]() mutable { h.resume(); });
        return true;
      }

      void await_resume() noexcept {}
    };

    return awaiter{this, std::move(t.ex)};
  }

  auto await_transform(this_coro::on_t t) noexcept {
    struct awaiter {
      any_executor target;

      bool await_ready() noexcept {
        IOCORO_ENSURE(target, "this_coro::on: empty executor");

        // Fast-path: if we're already running under the target executor, continue inline.
        // This avoids an unnecessary post() and keeps one-shot hops cheap for same-executor cases.
        if (detail::get_current_executor() == target) {
          return true;
        }
        return false;
      }

      auto await_suspend(std::coroutine_handle<> h) noexcept -> bool {
        target.post([h]() mutable { h.resume(); });
        return true;
      }

      void await_resume() noexcept {}
    };

    return awaiter{std::move(t.ex)};
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
    auto v = std::move(*value_);
    value_.reset();
    return v;
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

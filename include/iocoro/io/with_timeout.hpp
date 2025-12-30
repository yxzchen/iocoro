#pragma once

#include <iocoro/assert.hpp>
#include <iocoro/awaitable.hpp>
#include <iocoro/co_spawn.hpp>
#include <iocoro/completion_token.hpp>
#include <iocoro/detail/require_io_executor.hpp>
#include <iocoro/detail/unique_function.hpp>
#include <iocoro/error.hpp>
#include <iocoro/expected.hpp>
#include <iocoro/io/stream_concepts.hpp>
#include <iocoro/io_executor.hpp>
#include <iocoro/steady_timer.hpp>
#include <iocoro/this_coro.hpp>
#include <iocoro/executor.hpp>

#include <atomic>
#include <chrono>
#include <concepts>
#include <memory>
#include <optional>
#include <system_error>
#include <type_traits>
#include <utility>

namespace iocoro::io {

namespace detail {

template <class A>
struct with_timeout_result;

template <class T>
struct with_timeout_result<iocoro::awaitable<T>> {
  using type = T;
};

template <class Awaitable>
using with_timeout_result_t = typename with_timeout_result<std::remove_cvref_t<Awaitable>>::type;

template <class Result>
struct with_timeout_result_traits;

template <class T>
struct with_timeout_result_traits<iocoro::expected<T, std::error_code>> {
  using result_type = iocoro::expected<T, std::error_code>;

  static auto is_operation_aborted(result_type const& r) -> bool {
    if (!r && r.error() == error::operation_aborted) {
      return true;
    }
    return false;
  }

  static auto from_error(std::error_code ec) -> result_type {
    return unexpected(ec);
  }

  static auto timed_out() -> result_type {
    return unexpected(error::timed_out);
  }
};

template <>
struct with_timeout_result_traits<std::error_code> {
  using result_type = std::error_code;

  static auto is_operation_aborted(result_type const& r) -> bool {
    if (r == error::operation_aborted) {
      return true;
    }
    return false;
  }

  static auto from_error(std::error_code ec) -> result_type {
    return ec;
  }

  static auto timed_out() -> result_type {
    return error::timed_out;
  }
};

template <class T>
auto bind_executor(io_executor ex, awaitable<T> op) -> awaitable<T> {
  auto h = op.release();
  h.promise().set_executor(any_executor{ex});
  return awaitable<T>{h};
}

}  // namespace detail

/// Await an I/O awaitable with a deadline.
///
/// Semantics:
/// - **ex** is the unique anchor for timer + watcher + operation completion.
/// - The awaited operation is forced to complete back onto **ex**.
/// - When the timeout fires, this function:
///   - executes `on_timeout()` first
///   - then waits for `op` to finish (typically by being cancelled by `on_timeout()`).
template <class Awaitable, class OnTimeout>
  requires std::invocable<OnTimeout&> && requires { typename detail::with_timeout_result_t<Awaitable>; }
auto with_timeout(io_executor ex, Awaitable&& op, std::chrono::steady_clock::duration timeout,
                  OnTimeout&& on_timeout) -> awaitable<detail::with_timeout_result_t<Awaitable>> {
  using result_t = detail::with_timeout_result_t<Awaitable>;
  using traits = detail::with_timeout_result_traits<result_t>;

  IOCORO_ENSURE(ex, "with_timeout: requires a non-empty io_executor");

  if (timeout <= std::chrono::steady_clock::duration::zero()) {
    on_timeout();
    auto r = co_await std::move(op);
    if (traits::is_operation_aborted(r)) {
      co_return traits::timed_out();
    }
    co_return r;
  }

  auto timer = std::make_shared<steady_timer>(ex);
  (void)timer->expires_after(std::chrono::duration_cast<steady_timer::duration>(timeout));

  std::atomic<bool> fired{false};

  auto watcher = co_spawn(
    ex,
    [timer, &fired, on_timeout = std::forward<OnTimeout>(on_timeout)]() mutable -> awaitable<void> {
      auto ec = co_await timer->async_wait(use_awaitable);
      if (!ec) {
        fired.store(true, std::memory_order_release);
        on_timeout();
      }
      co_return;
    },
    use_awaitable);

  auto r = co_await std::move(op);

  (void)timer->cancel();
  (void)co_await std::move(watcher);

  if (fired.load(std::memory_order_acquire) && traits::is_operation_aborted(r)) {
    co_return traits::timed_out();
  }

  co_return r;
}

/// Syntax sugar 1: IO coroutine usage.
///
/// Strong requirement:
/// - The current coroutine MUST be running on io_executor.
/// - Otherwise this fails at runtime (require_io_executor).
///
/// This is purely:
/// - with_timeout(co_await this_coro::executor, ...)
/// and does not introduce any new semantics.
template <class Awaitable, class OnTimeout>
  requires std::invocable<OnTimeout&> && requires { typename detail::with_timeout_result_t<Awaitable>; }
auto with_timeout(Awaitable&& op, std::chrono::steady_clock::duration timeout, OnTimeout&& on_timeout)
  -> awaitable<detail::with_timeout_result_t<Awaitable>> {
  auto ex_any = co_await this_coro::executor;
  IOCORO_ENSURE(ex_any, "with_timeout: requires a bound executor");
  auto ex = ::iocoro::detail::require_io_executor(ex_any);
  co_return co_await with_timeout(ex, std::forward<Awaitable>(op), timeout, std::forward<OnTimeout>(on_timeout));
}

namespace detail {

template <class Stream>
concept io_executor_stream = requires(Stream& s) {
  { s.get_executor() } -> std::same_as<io_executor>;
};

}  // namespace detail

/// Syntax sugar 2: stream-bound.
///
/// Rules:
/// - executor source: s.get_executor() (must be io_executor)
/// - on_timeout is bound automatically (cancel/cancel_read/cancel_write)
template <class Stream, class Awaitable>
  requires cancellable_stream<Stream> && detail::io_executor_stream<Stream> &&
           requires { typename detail::with_timeout_result_t<Awaitable>; }
auto with_timeout(Stream& s, Awaitable&& op, std::chrono::steady_clock::duration timeout)
  -> awaitable<detail::with_timeout_result_t<Awaitable>> {
  co_return co_await with_timeout(s.get_executor(), std::forward<Awaitable>(op), timeout,
                                  [&]() { s.cancel(); });
}

template <class Stream, class Awaitable>
  requires cancellable_stream<Stream> && detail::io_executor_stream<Stream> &&
           requires { typename detail::with_timeout_result_t<Awaitable>; }
auto with_timeout_read(Stream& s, Awaitable&& op, std::chrono::steady_clock::duration timeout)
  -> awaitable<detail::with_timeout_result_t<Awaitable>> {
  if constexpr (cancel_readable_stream<Stream>) {
    co_return co_await with_timeout(s.get_executor(), std::forward<Awaitable>(op), timeout,
                                    [&]() { s.cancel_read(); });
  } else {
    co_return co_await with_timeout(s.get_executor(), std::forward<Awaitable>(op), timeout,
                                    [&]() { s.cancel(); });
  }
}

template <class Stream, class Awaitable>
  requires cancellable_stream<Stream> && detail::io_executor_stream<Stream> &&
           requires { typename detail::with_timeout_result_t<Awaitable>; }
auto with_timeout_write(Stream& s, Awaitable&& op, std::chrono::steady_clock::duration timeout)
  -> awaitable<detail::with_timeout_result_t<Awaitable>> {
  if constexpr (cancel_writable_stream<Stream>) {
    co_return co_await with_timeout(s.get_executor(), std::forward<Awaitable>(op), timeout,
                                    [&]() { s.cancel_write(); });
  } else {
    co_return co_await with_timeout(s.get_executor(), std::forward<Awaitable>(op), timeout,
                                    [&]() { s.cancel(); });
  }
}

/// Optional: detached variant.
///
/// Semantics:
/// - On timeout, executes on_timeout and returns timed_out without waiting op to complete.
/// - op may continue running on ex after this returns.
template <class Awaitable, class OnTimeout = decltype([] {})>
  requires std::invocable<OnTimeout&> && requires { typename detail::with_timeout_result_t<Awaitable>; }
auto with_timeout_detached(io_executor ex, Awaitable&& op, std::chrono::steady_clock::duration timeout,
                           OnTimeout&& on_timeout = OnTimeout{})
  -> awaitable<detail::with_timeout_result_t<Awaitable>> {
  using result_t = detail::with_timeout_result_t<Awaitable>;
  using traits = detail::with_timeout_result_traits<result_t>;

  IOCORO_ENSURE(ex, "with_timeout_detached: requires a non-empty io_executor");

  if (timeout <= std::chrono::steady_clock::duration::zero()) {
    on_timeout();
    co_return traits::timed_out();
  }

  struct state {
    io_executor ex;
    std::shared_ptr<steady_timer> timer;
    std::mutex m;
    bool completed = false;
    bool timer_won = false;
    bool timeout_fired = false;
    std::optional<result_t> value;
    std::exception_ptr ep;
    std::coroutine_handle<> waiter{};
    ::iocoro::detail::unique_function<void()> on_timeout;
  };

  auto st = std::make_shared<state>();
  st->ex = ex;
  st->timer = std::make_shared<steady_timer>(ex);
  st->on_timeout = ::iocoro::detail::unique_function<void()>(std::forward<OnTimeout>(on_timeout));
  (void)st->timer->expires_after(std::chrono::duration_cast<steady_timer::duration>(timeout));

  struct awaiter {
    std::shared_ptr<state> st;

    bool await_ready() const noexcept { return false; }

    bool await_suspend(std::coroutine_handle<> h) noexcept {
      std::coroutine_handle<> to_resume{};
      {
        std::scoped_lock lk{st->m};
        if (st->completed) {
          return false;
        }
        st->waiter = h;
      }
      (void)to_resume;
      return true;
    }

    void await_resume() const {
      if (st->timer_won) {
        return;
      }
      if (st->ep) {
        std::rethrow_exception(st->ep);
      }
    }
  };

  // Start the operation on ex and capture its completion.
  auto bound = detail::bind_executor<result_t>(ex, std::forward<Awaitable>(op));
  co_spawn(
    ex,
    std::move(bound),
    [st](expected<result_t, std::exception_ptr> r) mutable {
      std::coroutine_handle<> w{};
      {
        std::scoped_lock lk{st->m};
        if (st->completed) {
          return;
        }
        st->completed = true;
        st->timer_won = false;
        w = st->waiter;
        st->waiter = {};
      }

      (void)st->timer->cancel();

      if (!r) {
        st->ep = std::move(r).error();
      } else {
        st->value.emplace(std::move(*r));
      }

      if (w) {
        st->ex.post([w] { w.resume(); });
      }
    });

  // Start the timer on ex.
  co_spawn(
    ex,
    [timer = st->timer]() -> awaitable<std::error_code> {
      co_return co_await timer->async_wait(use_awaitable);
    },
    [st](expected<std::error_code, std::exception_ptr> r) mutable {
      std::coroutine_handle<> w{};
      bool fire = false;
      std::optional<result_t> timer_value{};
      {
        std::scoped_lock lk{st->m};
        if (st->completed) {
          return;
        }
        if (!r) {
          st->completed = true;
          st->timer_won = true;
          st->timeout_fired = false;
          timer_value.emplace(traits::from_error(error::operation_aborted));
          w = st->waiter;
          st->waiter = {};
        } else {
          auto ec = *r;
          if (!ec) {
            st->completed = true;
            st->timer_won = true;
            st->timeout_fired = true;
            fire = true;
            w = st->waiter;
            st->waiter = {};
          } else {
            st->completed = true;
            st->timer_won = true;
            st->timeout_fired = false;
            timer_value.emplace(traits::from_error(ec));
            w = st->waiter;
            st->waiter = {};
          }
        }
      }

      if (timer_value.has_value()) {
        st->value = std::move(timer_value);
      }

      if (fire) {
        st->on_timeout();
      }

      if (w) {
        st->ex.post([w] { w.resume(); });
      }
    });

  co_await awaiter{st};

  if (st->timer_won) {
    if (st->timeout_fired) {
      co_return traits::timed_out();
    }
    IOCORO_ENSURE(st->value.has_value(), "with_timeout_detached: missing timer error");
    co_return std::move(*st->value);
  }

  IOCORO_ENSURE(st->value.has_value(), "with_timeout_detached: missing value");
  co_return std::move(*st->value);
}

}  // namespace iocoro::io

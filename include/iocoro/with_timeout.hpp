#pragma once

#include <iocoro/assert.hpp>
#include <iocoro/awaitable.hpp>
#include <iocoro/completion_token.hpp>
#include <iocoro/co_spawn.hpp>
#include <iocoro/detail/when/when_state_base.hpp>
#include <iocoro/error.hpp>
#include <iocoro/result.hpp>
#include <iocoro/steady_timer.hpp>
#include <iocoro/this_coro.hpp>

#include <chrono>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <system_error>
#include <type_traits>
#include <utility>
#include <variant>

namespace iocoro {

namespace detail {

template <class T>
struct is_result_with_error_code : std::false_type {};

#if defined(__cpp_lib_expected) && __cpp_lib_expected >= 202211L
template <class U>
struct is_result_with_error_code<std::expected<U, std::error_code>> : std::true_type {};
#else
template <class U>
struct is_result_with_error_code<::iocoro::expected<U, std::error_code>> : std::true_type {};
#endif

template <class T>
inline constexpr bool is_result_with_error_code_v =
  is_result_with_error_code<std::remove_cvref_t<T>>::value;

}  // namespace detail

namespace detail {

template <class A, class B>
struct when_or_state : when_state_base {
  using result_variant = std::variant<when_value_t<A>, when_value_t<B>>;

  std::mutex result_m{};
  std::optional<result_variant> result{};
  std::size_t completed_index{0};
  awaitable<A> task_a;
  awaitable<B> task_b;

  explicit when_or_state(awaitable<A> a, awaitable<B> b)
      : when_state_base(1), task_a(std::move(a)), task_b(std::move(b)) {}

  template <std::size_t I, class V>
  void set_value(V&& v) {
    std::scoped_lock lk{result_m};
    completed_index = I;
    result.emplace(std::in_place_index<I>, std::forward<V>(v));
  }

  void request_cancel_other(std::size_t index) noexcept {
    if (index == 0) {
      task_b.request_stop();
      return;
    }
    task_a.request_stop();
  }
};

template <std::size_t I, class T, class A, class B>
auto when_or_run_one(std::shared_ptr<when_or_state<A, B>> st) -> awaitable<void> {
  try {
    if constexpr (std::is_void_v<T>) {
      if constexpr (I == 0) {
        co_await st->task_a;
      } else {
        co_await st->task_b;
      }
      if (st->try_complete()) {
        st->request_cancel_other(I);
        st->template set_value<I>(std::monostate{});
        st->complete();
      }
    } else {
      if constexpr (I == 0) {
        auto result = co_await st->task_a;
        if (st->try_complete()) {
          st->request_cancel_other(I);
          st->template set_value<I>(std::move(result));
          st->complete();
        }
      } else {
        auto result = co_await st->task_b;
        if (st->try_complete()) {
          st->request_cancel_other(I);
          st->template set_value<I>(std::move(result));
          st->complete();
        }
      }
    }
  } catch (...) {
    if (st->try_complete()) {
      st->request_cancel_other(I);
      st->set_exception(std::current_exception());
      st->complete();
    }
  }
  co_return;
}

}  // namespace detail

/// Wait for either awaitable to complete (binary when_any) and join the other.
///
/// Semantics:
/// - Starts both awaitables concurrently on their bound executors (or the caller's executor if
///   none is bound).
/// - Completes with `(index, value)` of the first completion.
/// - Requests stop on the non-winning awaitable (best-effort).
/// - Waits for the non-winning awaitable to finish after stop is requested ("cancel + join").
/// - If the first completion throws, still requests stop on the other awaitable, waits for both
///   runner coroutines to finish, and then rethrows the exception.
///
/// This is useful for implementing timeouts where the losing operation must be stopped and fully
/// completed before returning (to avoid lifetime hazards).
template <class A, class B>
auto when_any_cancel_join(awaitable<A> a, awaitable<B> b) -> awaitable<
  std::pair<std::size_t, std::variant<detail::when_value_t<A>, detail::when_value_t<B>>>> {
  auto fallback_ex = co_await this_coro::executor;
  IOCORO_ENSURE(fallback_ex, "when_any_cancel_join: requires a bound executor");
  auto parent_stop = co_await this_coro::stop_token;

  auto st = std::make_shared<detail::when_or_state<A, B>>(std::move(a), std::move(b));

  auto ex_a = st->task_a.get_executor();
  auto join_a = co_spawn(
    ex_a ? ex_a : fallback_ex, parent_stop,
    [st]() mutable -> awaitable<void> { return detail::when_or_run_one<0, A, A, B>(st); },
    use_awaitable);

  auto ex_b = st->task_b.get_executor();
  auto join_b = co_spawn(
    ex_b ? ex_b : fallback_ex, parent_stop,
    [st]() mutable -> awaitable<void> { return detail::when_or_run_one<1, B, A, B>(st); },
    use_awaitable);

  co_await detail::await_when(st);

  std::exception_ptr ep{};
  std::size_t index{};
  std::optional<typename detail::when_or_state<A, B>::result_variant> result{};
  {
    std::scoped_lock lk{st->result_m};
    ep = st->first_ep;
    if (!ep) {
      index = st->completed_index;
      result = std::move(st->result);
    }
  }

  if (ep) {
    co_await std::move(join_a);
    co_await std::move(join_b);
    std::rethrow_exception(ep);
  }

  if (index == 0U) {
    co_await std::move(join_b);
  } else {
    co_await std::move(join_a);
  }

  IOCORO_ENSURE(result.has_value(), "when_any_cancel_join: missing result");
  co_return std::make_pair(index, std::move(*result));
}

/// Await an operation with a timeout.
///
/// Semantics:
/// - Runs `op` concurrently with a timer.
/// - If `op` completes first, cancels the timer and returns `op`'s result.
/// - If the timer expires first, requests stop on `op`, waits for it to finish, and returns
///   `error::timed_out`.
///
/// Notes:
/// - This helper is intentionally constrained to `awaitable<result<...>>` so timeout can be
///   represented in the library's error model without double-wrapping.
template <class T, class Rep, class Period>
  requires detail::is_result_with_error_code_v<T>
auto with_timeout(awaitable<T> op, std::chrono::duration<Rep, Period> timeout) -> awaitable<T> {
  auto parent_stop = co_await this_coro::stop_token;
  if (parent_stop.stop_requested()) {
    co_return unexpected(error::operation_aborted);
  }

  auto io_ex = co_await this_coro::io_executor;
  IOCORO_ENSURE(io_ex, "with_timeout: requires a bound IO executor");

  steady_timer timer(io_ex);
  timer.expires_after(timeout);

  auto [index, v] = co_await when_any_cancel_join(std::move(op), timer.async_wait(use_awaitable));

  if (index == 0U) {
    timer.cancel();
    co_return std::move(std::get<0>(v));
  }

  // Timer completed first: distinguish natural expiry from cancellation due to stop.
  auto const& timer_res = std::get<1>(v);
  if (!timer_res) {
    if (timer_res.error() == make_error_code(error::operation_aborted) &&
        parent_stop.stop_requested()) {
      co_return unexpected(error::operation_aborted);
    }
    co_return unexpected(timer_res.error());
  }

  co_return unexpected(error::timed_out);
}

}  // namespace iocoro

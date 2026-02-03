#pragma once

#include <iocoro/awaitable.hpp>
#include <iocoro/awaitable_operators.hpp>
#include <iocoro/completion_token.hpp>
#include <iocoro/error.hpp>
#include <iocoro/result.hpp>
#include <iocoro/steady_timer.hpp>
#include <iocoro/this_coro.hpp>

#include <chrono>
#include <system_error>
#include <type_traits>

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

  auto [index, v] =
    co_await when_any_cancel_join(std::move(op), timer.async_wait(use_awaitable));

  if (index == 0U) {
    timer.cancel();
    co_return std::get<0>(v);
  }

  // Timer completed first: distinguish natural expiry from cancellation due to stop.
  auto const& timer_res = std::get<1>(v);
  if (!timer_res) {
    if (timer_res.error() == make_error_code(error::operation_aborted) && parent_stop.stop_requested()) {
      co_return unexpected(error::operation_aborted);
    }
    co_return unexpected(timer_res.error());
  }

  co_return unexpected(error::timed_out);
}

}  // namespace iocoro


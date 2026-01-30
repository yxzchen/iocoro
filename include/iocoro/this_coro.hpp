#pragma once

#include <iocoro/any_executor.hpp>
#include <iocoro/cancellation_token.hpp>
#include <iocoro/detail/executor_guard.hpp>

#include <chrono>
#include <utility>

namespace iocoro::this_coro {

struct executor_t {};
inline constexpr executor_t executor{};

inline auto get_executor() noexcept -> any_executor {
  return detail::get_current_executor();
}

struct switch_to_t {
  any_executor ex;
};

/// Switch the current coroutine to resume on the given executor.
///
/// Usage:
///   co_await iocoro::this_coro::switch_to(ex);
inline auto switch_to(any_executor ex) noexcept -> switch_to_t {
  return switch_to_t{std::move(ex)};
}

struct cancellation_token_t {};
inline constexpr cancellation_token_t cancellation_token{};

struct set_cancellation_token_t {
  ::iocoro::cancellation_token tok;
};

inline auto set_cancellation_token(::iocoro::cancellation_token tok) noexcept
  -> set_cancellation_token_t {
  return set_cancellation_token_t{std::move(tok)};
}

template <class Rep, class Period>
struct scoped_timeout_t {
  std::chrono::duration<Rep, Period> timeout;
};

template <class Rep, class Period>
inline auto scoped_timeout(std::chrono::duration<Rep, Period> timeout) noexcept
  -> scoped_timeout_t<Rep, Period> {
  return scoped_timeout_t<Rep, Period>{timeout};
}

}  // namespace iocoro::this_coro

#pragma once

#include <iocoro/any_executor.hpp>
#include <stop_token>

#include <chrono>
#include <utility>

namespace iocoro::this_coro {

struct executor_t {};
inline constexpr executor_t executor{};

struct io_executor_t {};
inline constexpr io_executor_t io_executor{};

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

struct stop_token_t {};
inline constexpr stop_token_t stop_token{};

struct set_stop_token_t {
  std::stop_token tok;
};

inline auto set_stop_token(std::stop_token tok) noexcept -> set_stop_token_t {
  return set_stop_token_t{std::move(tok)};
}

template <class Rep, class Period>
struct scoped_timeout_t {
  any_executor timer_ex{};
  std::chrono::duration<Rep, Period> timeout;
};

template <class Rep, class Period>
inline auto scoped_timeout(std::chrono::duration<Rep, Period> timeout) noexcept
  -> scoped_timeout_t<Rep, Period> {
  return scoped_timeout_t<Rep, Period>{any_executor{}, timeout};
}

template <class Rep, class Period>
inline auto scoped_timeout(any_executor timer_ex,
                           std::chrono::duration<Rep, Period> timeout) noexcept
  -> scoped_timeout_t<Rep, Period> {
  return scoped_timeout_t<Rep, Period>{std::move(timer_ex), timeout};
}

}  // namespace iocoro::this_coro

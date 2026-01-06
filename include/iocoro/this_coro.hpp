#pragma once

#include <iocoro/any_executor.hpp>
#include <iocoro/detail/executor_guard.hpp>

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

}  // namespace iocoro::this_coro

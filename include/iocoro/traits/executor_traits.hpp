#pragma once

#include <iocoro/detail/unique_function.hpp>

#include <concepts>
#include <cstdint>
#include <type_traits>
#include <utility>

namespace iocoro {

namespace detail {
class io_context_impl;
}  // namespace detail

/// Capability flags that may be reported by an executor.
///
/// This is primarily used by `any_io_executor` to validate that the erased executor
/// is IO-capable (and to extract the associated `io_context_impl*` when available).
enum class executor_capability : std::uint8_t {
  none = 0,
  io = 1 << 0,
};

inline constexpr auto operator|(executor_capability lhs, executor_capability rhs) noexcept
  -> executor_capability {
  return static_cast<executor_capability>(static_cast<std::uint8_t>(lhs) |
                                          static_cast<std::uint8_t>(rhs));
}

inline constexpr auto operator&(executor_capability lhs, executor_capability rhs) noexcept
  -> executor_capability {
  return static_cast<executor_capability>(static_cast<std::uint8_t>(lhs) &
                                          static_cast<std::uint8_t>(rhs));
}

inline constexpr auto has_capability(executor_capability caps, executor_capability flag) noexcept
  -> bool {
  return (caps & flag) != executor_capability::none;
}

/// Minimal executor concept required by IOCoro.
template <class Ex>
concept executor = requires(Ex ex, detail::unique_function<void()> fn) {
  { ex.post(std::move(fn)) } noexcept;
  { ex.dispatch(std::move(fn)) } noexcept;
  { std::as_const(ex) == std::as_const(ex) } -> std::convertible_to<bool>;
};

namespace detail {

/// Customization point used by `any_executor` type-erasure to query optional executor metadata.
///
/// Concrete executors can specialize this trait to:
/// - report `executor_capability::io` when IO-capable
/// - return a non-null `io_context_impl*` when associated with an io_context
template <class Ex>
struct executor_traits {
  static auto capabilities(Ex const&) noexcept -> iocoro::executor_capability {
    return executor_capability::none;
  }

  static auto io_context(Ex const&) noexcept -> io_context_impl* { return nullptr; }
};

}  // namespace detail

}  // namespace iocoro

#pragma once

#include <iocoro/error.hpp>
#include <iocoro/expected.hpp>

#include <system_error>

namespace iocoro::traits {

/// Type trait to handle timeout result types and their error conditions.
///
/// Provides a uniform interface for checking operation_aborted status,
/// creating error results, and generating timed_out results across
/// different result types (expected<T, error_code> and error_code).
template <class Result>
struct timeout_result_traits;

/// Specialization for expected<T, error_code> result types.
template <class T>
struct timeout_result_traits<iocoro::expected<T, std::error_code>> {
  using result_type = iocoro::expected<T, std::error_code>;

  static auto is_operation_aborted(result_type const& r) -> bool {
    if (!r && r.error() == error::operation_aborted) {
      return true;
    }
    return false;
  }

  static auto from_error(std::error_code ec) -> result_type { return unexpected(ec); }

  static auto timed_out() -> result_type { return unexpected(error::timed_out); }
};

/// Specialization for error_code result types.
template <>
struct timeout_result_traits<std::error_code> {
  using result_type = std::error_code;

  static auto is_operation_aborted(result_type const& r) -> bool {
    if (r == error::operation_aborted) {
      return true;
    }
    return false;
  }

  static auto from_error(std::error_code ec) -> result_type { return ec; }

  static auto timed_out() -> result_type { return error::timed_out; }
};

}  // namespace iocoro::traits

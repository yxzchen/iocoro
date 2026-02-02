#pragma once

#include <iocoro/expected.hpp>

#include <system_error>
#include <variant>

namespace iocoro {

/// Common result type for IO-style APIs.
template <class T>
using io_result = expected<T, std::error_code>;

/// Result type for void-returning operations.
/// (iocoro::expected does not support T=void in the fallback implementation.)
using void_result = expected<std::monostate, std::error_code>;

[[nodiscard]] inline auto ok() noexcept -> void_result { return std::monostate{}; }
[[nodiscard]] inline auto fail(std::error_code ec) noexcept -> void_result { return unexpected(ec); }

}  // namespace iocoro


#pragma once

#include <iocoro/expected.hpp>

#include <system_error>

namespace iocoro {

/// Common result type for error_code-based APIs.
template <class T>
using result = expected<T, std::error_code>;

/// Backward-compatible alias (kept for clarity in IO-heavy APIs).
template <class T>
using io_result = result<T>;

/// Result type for void-returning operations.
using void_result = expected<void, std::error_code>;

[[nodiscard]] inline auto ok() noexcept -> void_result { return {}; }
[[nodiscard]] inline auto fail(std::error_code ec) noexcept -> void_result { return unexpected(ec); }

}  // namespace iocoro


#pragma once

#include <iocoro/expected.hpp>

#include <system_error>

namespace iocoro {

/// Common result type for error_code-based APIs.
template <class T>
using result = expected<T, std::error_code>;

[[nodiscard]] inline auto ok() noexcept -> result<void> {
  return {};
}
[[nodiscard]] inline auto fail(std::error_code ec) noexcept -> result<void> {
  return unexpected(ec);
}

}  // namespace iocoro

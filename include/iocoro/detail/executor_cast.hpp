#pragma once

#include <iocoro/any_executor.hpp>
#include <iocoro/any_io_executor.hpp>

namespace iocoro::detail {

struct any_executor_access {
  template <class T>
  static auto target(any_executor const& ex) noexcept -> T const* {
    return ex.target<T>();
  }

  static auto io_context(any_executor const& ex) noexcept -> io_context_impl* {
    return ex.io_context_ptr();
  }
};

[[nodiscard]] inline auto to_io_executor(any_executor const& ex) noexcept -> any_io_executor {
  if (!ex) {
    return any_io_executor{};
  }
  if (!ex.supports_io()) {
    return any_io_executor{};
  }
  if (ex.io_context_ptr() == nullptr) {
    return any_io_executor{};
  }
  return any_io_executor{ex};
}

}  // namespace iocoro::detail

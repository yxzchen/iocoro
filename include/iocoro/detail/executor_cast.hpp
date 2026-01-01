#pragma once

#include <iocoro/assert.hpp>
#include <iocoro/executor.hpp>
#include <iocoro/io_executor.hpp>

namespace iocoro::detail {

struct any_executor_access {
  template <class T>
  static auto target(any_executor const& ex) noexcept -> T const* {
    return ex.target<T>();
  }
};

/// Require any_executor to be of specific type, abort if not.
/// @throws assertion failure if executor is not of target type or is empty.
template <executor Target>
inline auto require_executor(any_executor const& ex) noexcept -> Target {
  auto const* p = any_executor_access::target<Target>(ex);
  IOCORO_ENSURE(p != nullptr, "require_executor: executor is not of required type");
  return *p;
}

}  // namespace iocoro::detail



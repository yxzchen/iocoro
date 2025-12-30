#pragma once

#include <iocoro/assert.hpp>
#include <iocoro/detail/any_executor_access.hpp>
#include <iocoro/io_executor.hpp>

namespace iocoro::detail {

inline auto require_io_executor(any_executor const& ex) noexcept -> io_executor {
  auto const* p = any_executor_access::target<io_executor>(ex);
  IOCORO_ENSURE(p != nullptr, "require_io_executor: executor is not an io_executor");
  IOCORO_ENSURE(*p, "require_io_executor: empty io_executor");
  return *p;
}

}  // namespace iocoro::detail



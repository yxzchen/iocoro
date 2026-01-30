#pragma once

#include <iocoro/assert.hpp>
#include <iocoro/any_executor.hpp>

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

struct reactor_access {
  io_context_impl* impl{};

  auto valid() const noexcept -> bool { return impl != nullptr; }
  explicit operator bool() const noexcept { return valid(); }

  auto get() const noexcept -> io_context_impl& {
    IOCORO_ENSURE(impl, "reactor_access: null io_context_impl");
    return *impl;
  }
};

inline auto get_reactor_access(any_executor const& ex) noexcept -> reactor_access {
  return reactor_access{any_executor_access::io_context(ex)};
}

/// Require any_executor to be of specific type, abort if not.
/// @throws assertion failure if executor is not of target type or is empty.
template <executor Target>
inline auto require_executor(any_executor const& ex) noexcept -> Target {
  IOCORO_ENSURE(ex, "require_executor: requires a valid executor");
  auto const* p = any_executor_access::target<Target>(ex);
  IOCORO_ENSURE(p, "require_executor: executor is not of required type");
  return *p;
}

}  // namespace iocoro::detail



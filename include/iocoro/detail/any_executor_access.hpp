#pragma once

#include <iocoro/executor.hpp>

namespace iocoro::detail {

/// Internal access to type-erased executors.
///
/// This is intentionally in `detail/` to avoid making type queries part of the
/// public executor contract. Library internals may use it to require a specific
/// executor type (e.g. io_executor) when interacting with IO-bound components.
struct any_executor_access {
  template <class T>
  static auto target(any_executor const& ex) noexcept -> T const* {
    return ex.target<T>();
  }
};

}  // namespace iocoro::detail



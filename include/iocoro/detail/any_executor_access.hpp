#pragma once

#include <iocoro/executor.hpp>

namespace iocoro::detail {

struct any_executor_access {
  template <class T>
  static auto target(any_executor const& ex) noexcept -> T const* {
    return ex.target<T>();
  }
};

}  // namespace iocoro::detail



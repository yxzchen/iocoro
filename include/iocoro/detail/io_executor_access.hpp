#pragma once

#include <iocoro/io_executor.hpp>

namespace iocoro::detail {

struct io_executor_access {
  static auto impl(io_executor const& ex) noexcept -> io_context_impl* { return ex.impl_; }
};

}  // namespace iocoro::detail


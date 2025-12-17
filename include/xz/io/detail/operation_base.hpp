#pragma once

#include <memory>
#include <system_error>

namespace xz::io {

/// Base class for I/O operation callbacks registered with io_context
struct operation_base {
  virtual ~operation_base() = default;
  virtual void execute() = 0;
  virtual void abort(std::error_code ec) = 0;
};

}  // namespace xz::io

#pragma once

#include <xz/io/detail/context/io_context_impl.hpp>
#include <xz/io/executor.hpp>

#include <memory>

namespace xz::io::detail {

/// Base class for low-level operations registered with an io_context_impl.
///
/// Design intent:
/// - `executor` is a construction-time "carrier" that grants access to the underlying
///   `io_context_impl`.
/// - During execution, operations talk directly to `io_context_impl` via `impl_`.
struct operation_base {
  operation_base(operation_base const&) = delete;
  auto operator=(operation_base const&) -> operation_base& = delete;
  operation_base(operation_base&&) = delete;
  auto operator=(operation_base&&) -> operation_base& = delete;

  virtual ~operation_base() = default;

  /// Start the operation by registering it with the underlying reactor.
  virtual void start() = 0;

  virtual void execute() = 0;
  virtual void abort(std::error_code ec) = 0;

 protected:
  explicit operation_base(xz::io::executor const& ex) noexcept : impl_{ex.impl_} {}

  io_context_impl* impl_;
};

/// Example: register interest in readability for a file descriptor.
///
/// IMPORTANT: `start()` transfers ownership of `this` into `io_context_impl` via a `unique_ptr`.
/// The object must therefore be created with `new` and must not be accessed after calling start().
class read_operation final : public operation_base {
 public:
  read_operation(int fd, xz::io::executor const& ex) noexcept : operation_base(ex), fd_{fd} {}

  void start() override { impl_->register_fd_read(fd_, std::unique_ptr<operation_base>(this)); }

 private:
  int fd_;
};

}  // namespace xz::io::detail

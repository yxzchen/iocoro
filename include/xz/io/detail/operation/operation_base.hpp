#pragma once

#include <xz/io/assert.hpp>
#include <xz/io/detail/context/io_context_impl.hpp>
#include <xz/io/executor.hpp>

#include <memory>
#include <system_error>

namespace xz::io::detail {

/// Base class for low-level operations registered with an io_context_impl.
///
/// Design intent:
/// - `executor` is a construction-time "carrier" that grants access to the underlying
///   `io_context_impl`.
/// - During execution, operations talk directly to `io_context_impl` via `impl_`.
class operation_base {
 public:
  operation_base(operation_base const&) = delete;
  auto operator=(operation_base const&) -> operation_base& = delete;
  operation_base(operation_base&&) = delete;
  auto operator=(operation_base&&) -> operation_base& = delete;

  virtual ~operation_base() = default;

  /// Start the operation by registering it with the underlying reactor.
  ///
  /// Ownership-transfer is centralized here: the reactor takes over `self`
  /// and will eventually complete / abort it.
  void start(std::unique_ptr<operation_base> self) {
    XZ_ENSURE(self.get() == this, "operation_base: start(self) self must own *this");
    do_start(std::move(self));
  }

  virtual void execute() = 0;
  virtual void abort(std::error_code ec) = 0;

 protected:
  explicit operation_base(xz::io::executor const& ex) noexcept : impl_{ex.impl_} {
    XZ_ENSURE(impl_ != nullptr, "operation_base: executor has null impl_");
  }

  io_context_impl* impl_;

 private:
  /// Derived classes only implement the registration action.
  virtual void do_start(std::unique_ptr<operation_base> self) = 0;
};

// auto op = std::make_unique<read_operation>(fd, ex);
// ex.dispatch([op = std::move(op)]() mutable {
//   op->start(std::move(op));
// });

class read_operation final : public operation_base {
 public:
  read_operation(int fd, xz::io::executor const& ex) noexcept : operation_base(ex), fd_{fd} {}

  void do_start(std::unique_ptr<operation_base> self) override {
    (void)impl_->register_fd_read(fd_, std::move(self));
  }

  void execute() override {}
  void abort(std::error_code) override {}

 private:
  int fd_;
};

}  // namespace xz::io::detail

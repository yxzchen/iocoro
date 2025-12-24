#pragma once

#include <iocoro/assert.hpp>
#include <iocoro/detail/io_context_impl.hpp>
#include <iocoro/executor.hpp>

#include <memory>
#include <system_error>

namespace iocoro::detail {

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
    IOCORO_ENSURE(self.get() == this, "operation_base: start(self) self must own *this");
    do_start(std::move(self));
  }

  virtual void execute() = 0;
  virtual void abort(std::error_code ec) = 0;

 protected:
  explicit operation_base(iocoro::executor const& ex) noexcept : impl_{ex.impl_} {
    IOCORO_ENSURE(impl_ != nullptr, "operation_base: executor has null impl_");
  }

  io_context_impl* impl_;

 private:
  /// Derived classes only implement the registration action.
  virtual void do_start(std::unique_ptr<operation_base> self) = 0;
};

}  // namespace iocoro::detail

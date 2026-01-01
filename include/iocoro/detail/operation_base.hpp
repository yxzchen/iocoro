#pragma once

#include <iocoro/assert.hpp>

#include <memory>
#include <system_error>

namespace iocoro::detail {

// Forward declaration
class io_context_impl;

/// Base class for low-level operations registered with an io_context_impl.
///
/// Design intent:
/// - operation_base is a pure reactor-layer object.
/// - It only provides callbacks for reactor events.
/// - It does NOT know about executors, coroutines, or completion handlers.
/// - Derived classes are responsible for bridging to higher-level abstractions.
class operation_base {
 public:
  operation_base() noexcept = default;

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

  /// Called by reactor when the operation becomes ready.
  virtual void on_ready() noexcept = 0;

  /// Called by reactor when the operation is cancelled or aborted.
  virtual void on_abort(std::error_code ec) noexcept = 0;

 private:
  /// Derived classes only implement the registration action.
  virtual void do_start(std::unique_ptr<operation_base> self) = 0;
};

}  // namespace iocoro::detail

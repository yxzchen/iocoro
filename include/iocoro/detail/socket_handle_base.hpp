#pragma once

#include <iocoro/io_executor.hpp>
#include <iocoro/io_context.hpp>
#include <iocoro/socket_option.hpp>

#include <memory>

namespace iocoro::detail {

/// A minimal, reusable PImpl wrapper for socket-like I/O handles.
///
/// Responsibilities:
/// - Own and share an implementation object (`Impl`) via `std::shared_ptr`.
/// - Provide common "handle" operations: io_executor access, open state, cancel/close,
///   socket options, and native_handle.
///
/// Non-responsibilities:
/// - This type intentionally does NOT encode any network protocol semantics.
///   Higher-level networking facades live under `iocoro::ip::basic_*<Protocol>`.
template <class Impl>
class socket_handle_base {
 public:
  using impl_type = Impl;

  /// Handles must be bound to an io_executor at construction time.
  socket_handle_base() = delete;

  explicit socket_handle_base(io_executor ex) : impl_(std::make_shared<Impl>(ex)) {}
  explicit socket_handle_base(io_context& ctx) : socket_handle_base(ctx.get_executor()) {}

  socket_handle_base(socket_handle_base const&) = delete;
  socket_handle_base& operator=(socket_handle_base const&) = delete;

  /// "Move" is intentionally implemented as shared-ownership transfer (copy the shared_ptr)
  /// so the moved-from handle remains usable and retains a valid impl object.
  ///
  /// This keeps the invariant: impl_ is never null for any handle object.
  socket_handle_base(socket_handle_base&& other) noexcept : impl_(other.impl_) {}
  socket_handle_base& operator=(socket_handle_base&& other) noexcept {
    if (this != &other) {
      impl_ = other.impl_;
    }
    return *this;
  }

  auto get_io_context_impl() const noexcept -> io_context_impl* { return impl_->get_io_context_impl(); }
  auto get_executor() const noexcept -> io_executor { return io_executor{*impl_->get_io_context_impl()}; }

  auto is_open() const noexcept -> bool { return impl_->is_open(); }

  void cancel() noexcept { impl_->cancel(); }
  void cancel_read() noexcept { impl_->cancel_read(); }
  void cancel_write() noexcept { impl_->cancel_write(); }

  void close() noexcept { impl_->close(); }

  template <class Option>
  auto set_option(Option const& opt) -> std::error_code {
    return impl_->set_option(opt);
  }

  template <class Option>
  auto get_option(Option& opt) -> std::error_code {
    return impl_->get_option(opt);
  }

  auto native_handle() const noexcept -> int { return impl_->native_handle(); }

 protected:
  std::shared_ptr<Impl> impl_;
};

}  // namespace iocoro::detail

#pragma once

#include <iocoro/any_io_executor.hpp>
#include <iocoro/io_context.hpp>
#include <iocoro/result.hpp>
#include <iocoro/socket_option.hpp>

#include <memory>
#include <system_error>

namespace iocoro::detail {

/// A minimal, reusable PImpl wrapper for socket-like I/O handles.
///
/// Responsibilities:
/// - Own an implementation object (`Impl`) via `std::unique_ptr`.
/// - Provide common "handle" operations: IO executor access, open state, cancel/close,
///   socket options, and native_handle.
///
/// Non-responsibilities:
/// - This type intentionally does NOT encode any network protocol semantics.
///   Higher-level networking facades live under `iocoro::ip::basic_*<Protocol>`.
template <class Impl>
class socket_handle_base {
 public:
  using impl_type = Impl;

  /// Handles must be bound to an IO-capable executor at construction time.
  socket_handle_base() = delete;

  explicit socket_handle_base(any_io_executor ex)
      : impl_(std::make_unique<Impl>(ex)), ex_(std::move(ex)) {}
  explicit socket_handle_base(io_context& ctx) : socket_handle_base(ctx.get_executor()) {}

  socket_handle_base(socket_handle_base const&) = delete;
  socket_handle_base& operator=(socket_handle_base const&) = delete;

  socket_handle_base(socket_handle_base&& other) noexcept = default;
  socket_handle_base& operator=(socket_handle_base&& other) noexcept = default;

  auto get_io_context_impl() const noexcept -> io_context_impl* {
    return impl_->get_io_context_impl();
  }
  auto get_executor() const noexcept -> any_io_executor { return ex_; }

  auto is_open() const noexcept -> bool { return impl_->is_open(); }

  void cancel() noexcept { impl_->cancel(); }
  void cancel_read() noexcept { impl_->cancel_read(); }
  void cancel_write() noexcept { impl_->cancel_write(); }

  auto close() noexcept -> result<void> { return impl_->close(); }

  template <class Option>
  auto set_option(Option const& opt) -> result<void> {
    return impl_->set_option(opt);
  }

  template <class Option>
  auto get_option(Option& opt) -> result<void> {
    return impl_->get_option(opt);
  }

  auto native_handle() const noexcept -> int { return impl_->native_handle(); }

  auto impl() noexcept -> Impl& { return *impl_; }
  auto impl() const noexcept -> Impl const& { return *impl_; }
  auto impl_ptr() noexcept -> Impl* { return impl_.get(); }
  auto impl_ptr() const noexcept -> Impl const* { return impl_.get(); }

 protected:
  std::unique_ptr<Impl> impl_;
  any_io_executor ex_{};
};

}  // namespace iocoro::detail

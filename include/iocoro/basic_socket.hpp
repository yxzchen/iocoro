#pragma once

#include <iocoro/executor.hpp>
#include <iocoro/io_context.hpp>
#include <iocoro/socket_option.hpp>

#include <memory>

namespace iocoro {

/// A minimal, reusable PImpl wrapper for socket-like public types.
///
/// Notes:
/// - This is intentionally thin and does not define protocol operations.
/// - Protocol-specific sockets (e.g. ip::tcp::socket) can wrap an implementation type
///   and forward operations.
template <class Impl>
class basic_socket {
 public:
  using impl_type = Impl;

  /// Sockets must be bound to an executor at construction time.
  basic_socket() = delete;

  explicit basic_socket(executor ex) : impl_(std::make_shared<Impl>(ex)) {}
  explicit basic_socket(io_context& ctx) : basic_socket(ctx.get_executor()) {}

  basic_socket(basic_socket const&) = delete;
  basic_socket& operator=(basic_socket const&) = delete;

  /// "Move" is intentionally implemented as shared-ownership transfer (copy the shared_ptr)
  /// so the moved-from socket remains usable and retains a valid impl object.
  ///
  /// This keeps the invariant: impl_ is never null for any socket object.
  basic_socket(basic_socket&& other) noexcept : impl_(other.impl_) {}
  basic_socket& operator=(basic_socket&& other) noexcept {
    if (this != &other) {
      impl_ = other.impl_;
    }
    return *this;
  }

  auto get_executor() const noexcept -> executor { return impl_->get_executor(); }

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

}  // namespace iocoro

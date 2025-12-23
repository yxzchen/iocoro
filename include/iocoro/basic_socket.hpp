#pragma once

#include <iocoro/io_context.hpp>
#include <iocoro/socket_option.hpp>

#include <iocoro/executor.hpp>

#include <memory>

namespace iocoro {

/// A minimal, reusable PImpl wrapper for socket-like public types.
///
/// Notes:
/// - This is intentionally thin and does not define protocol operations.
/// - Protocol-specific sockets (e.g. ip::tcp_socket) can wrap an implementation type
///   and forward operations.
template <class Impl>
class basic_socket {
 public:
  using impl_type = Impl;

  basic_socket() noexcept = default;

  explicit basic_socket(executor ex) : impl_(std::make_shared<Impl>(ex)) {}
  explicit basic_socket(io_context& ctx) : basic_socket(ctx.get_executor()) {}

  auto get_executor() const noexcept -> executor {
    return impl_ ? impl_->get_executor() : executor{};
  }
  auto is_open() const noexcept -> bool { return impl_ && impl_->is_open(); }
  auto native_handle() const noexcept -> int { return impl_ ? impl_->native_handle() : -1; }

  void cancel() noexcept {
    if (impl_) impl_->cancel();
  }
  void close() noexcept {
    if (impl_) impl_->close();
  }

 protected:
  std::shared_ptr<Impl> impl_{};
};

}  // namespace iocoro

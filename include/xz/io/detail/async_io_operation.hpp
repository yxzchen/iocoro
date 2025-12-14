#pragma once

#include <xz/io/awaitable.hpp>
#include <xz/io/io_context.hpp>

#include <chrono>
#include <memory>

namespace xz::io {

namespace detail {
class tcp_socket_impl;
}  // namespace detail

/// Base class template for asynchronous I/O operations with timeout support
template <typename Result>
class async_io_operation : public awaitable_op<Result> {
 protected:
  std::weak_ptr<detail::tcp_socket_impl> socket_impl_;
  std::chrono::milliseconds timeout_;
  detail::timer_handle timer_handle_;

  void setup_timeout();
  void cleanup_timer();

  auto get_socket_impl() const -> std::shared_ptr<detail::tcp_socket_impl> { return socket_impl_.lock(); }

 public:
  async_io_operation(std::weak_ptr<detail::tcp_socket_impl> socket_impl, std::chrono::milliseconds timeout)
      : socket_impl_(std::move(socket_impl)), timeout_(timeout) {}
};

}  // namespace xz::io

#pragma once

#include <xz/io/detail/async_io_operation.hpp>
#include <xz/io/ip.hpp>

#include <memory>
#include <chrono>

namespace xz::io {

namespace detail {
class tcp_socket_impl;
}  // namespace detail

/// Asynchronous connect operation
class tcp_socket;

struct [[nodiscard]] async_connect_op : async_io_operation<void> {
  async_connect_op(std::weak_ptr<detail::tcp_socket_impl> socket_impl, ip::tcp_endpoint ep,
                   std::chrono::milliseconds timeout = {});

 protected:
  void start_operation() override;

 private:
  ip::tcp_endpoint endpoint_;

  friend class tcp_socket;
};

}  // namespace xz::io

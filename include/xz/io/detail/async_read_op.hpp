#pragma once

#include <xz/io/detail/async_io_operation.hpp>
#include <xz/io/expected.hpp>

#include <memory>
#include <chrono>
#include <span>

namespace xz::io {

namespace detail {
class tcp_socket_impl;
}  // namespace detail

/// Asynchronous read operation
class tcp_socket;

struct [[nodiscard]] async_read_some_op : async_io_operation<std::size_t> {
  async_read_some_op(std::weak_ptr<detail::tcp_socket_impl> socket_impl, std::span<char> buf,
                     std::chrono::milliseconds timeout = {});

 protected:
  void start_operation() override;

 private:
  std::span<char> buffer_;

  friend class tcp_socket;
};

}  // namespace xz::io

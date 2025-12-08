#pragma once

#include <xz/io/awaitable.hpp>
#include <xz/io/io_context.hpp>
#include <xz/io/ip.hpp>

#include <chrono>
#include <memory>
#include <span>
#include <system_error>

namespace xz::io {

namespace detail {
class tcp_socket_impl;
}

/// Asynchronous TCP socket
class tcp_socket {
 public:
  explicit tcp_socket(io_context& ctx);
  ~tcp_socket();

  // Non-copyable, movable
  tcp_socket(tcp_socket const&) = delete;
  auto operator=(tcp_socket const&) -> tcp_socket& = delete;
  tcp_socket(tcp_socket&&) noexcept;
  auto operator=(tcp_socket&&) noexcept -> tcp_socket&;

  /// Get the associated io_context
  auto get_executor() noexcept -> io_context&;

  /// Check if socket is open
  auto is_open() const noexcept -> bool;

  /// Get native handle
  auto native_handle() const noexcept -> int;

  /// Close the socket
  void close();
  void close(std::error_code& ec) noexcept;

  /// Async operations

  /// Connect to remote endpoint (coroutine-based)
  struct [[nodiscard]] async_connect_op : awaitable_op<void> {
    async_connect_op(tcp_socket& s, ip::tcp_endpoint ep, std::chrono::milliseconds timeout = {});

   protected:
    void start_operation() override;

   private:
    tcp_socket& socket_;
    ip::tcp_endpoint endpoint_;
    std::chrono::milliseconds timeout_;
  };

  auto async_connect(ip::tcp_endpoint ep, std::chrono::milliseconds timeout = {}) -> async_connect_op {
    return async_connect_op{*this, ep, timeout};
  }

  /// Read some data (coroutine-based)
  struct [[nodiscard]] async_read_some_op : awaitable_op<std::size_t> {
    async_read_some_op(tcp_socket& s, std::span<char> buf);

   protected:
    void start_operation() override;

   private:
    tcp_socket& socket_;
    std::span<char> buffer_;
  };

  auto async_read_some(std::span<char> buffer) -> async_read_some_op {
    return async_read_some_op{*this, buffer};
  }

  /// Write some data (coroutine-based)
  struct [[nodiscard]] async_write_some_op : awaitable_op<std::size_t> {
    async_write_some_op(tcp_socket& s, std::span<char const> buf);

   protected:
    void start_operation() override;

   private:
    tcp_socket& socket_;
    std::span<char const> buffer_;
  };

  auto async_write_some(std::span<char const> buffer) -> async_write_some_op {
    return async_write_some_op{*this, buffer};
  }

  /// Socket options
  void set_option_nodelay(bool enable);
  void set_option_keepalive(bool enable);
  void set_option_reuseaddr(bool enable);

  /// Local/remote endpoints
  auto local_endpoint() const -> ip::tcp_endpoint;
  auto remote_endpoint() const -> ip::tcp_endpoint;

 private:
  friend struct async_connect_op;
  friend struct async_read_some_op;
  friend struct async_write_some_op;

  std::unique_ptr<detail::tcp_socket_impl> impl_;
};

/// Free functions for full read/write operations

/// Read exactly n bytes
inline auto async_read(tcp_socket& s, std::span<char> buffer) -> task<void> {
  std::size_t total = 0;
  while (total < buffer.size()) {
    auto n = co_await s.async_read_some(buffer.subspan(total));
    if (n == 0) throw std::system_error(make_error_code(error::eof));
    total += n;
  }
}

/// Write all data
inline auto async_write(tcp_socket& s, std::span<char const> buffer) -> task<void> {
  std::size_t total = 0;
  while (total < buffer.size()) {
    auto n = co_await s.async_write_some(buffer.subspan(total));
    total += n;
  }
}

}  // namespace xz::io

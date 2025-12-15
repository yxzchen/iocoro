#pragma once

#include <xz/io/awaitable.hpp>
#include <xz/io/expected.hpp>
#include <xz/io/io_context.hpp>
#include <xz/io/ip.hpp>

#include <chrono>
#include <memory>
#include <span>
#include <system_error>

namespace xz::io {

namespace detail {
class tcp_socket_impl;
}  // namespace detail

/// Forward declaration
class tcp_socket;

/// Asynchronous TCP socket
class tcp_socket {
 public:
  explicit tcp_socket(io_context& ctx);
  ~tcp_socket();

  tcp_socket(tcp_socket const&) = delete;
  auto operator=(tcp_socket const&) -> tcp_socket& = delete;
  tcp_socket(tcp_socket&&) noexcept;
  auto operator=(tcp_socket&&) noexcept -> tcp_socket&;

  auto get_executor() noexcept -> io_context&;

  auto is_open() const noexcept -> bool;

  auto native_handle() const noexcept -> int;

  void close();
  auto close_nothrow() noexcept -> std::error_code;

  /// Async operations

  auto async_connect(ip::tcp_endpoint ep, std::chrono::milliseconds timeout = {}) -> awaitable<void>;

  auto async_read_some(std::span<char> buffer, std::chrono::milliseconds timeout = {}) -> awaitable<std::size_t>;

  auto async_write_some(std::span<char const> buffer, std::chrono::milliseconds timeout = {}) -> awaitable<std::size_t>;

  auto set_option_nodelay(bool enable) -> std::error_code;
  auto set_option_keepalive(bool enable) -> std::error_code;
  auto set_option_reuseaddr(bool enable) -> std::error_code;

  auto local_endpoint() const -> expected<ip::tcp_endpoint, std::error_code>;
  auto remote_endpoint() const -> expected<ip::tcp_endpoint, std::error_code>;

 private:
  std::shared_ptr<detail::tcp_socket_impl> socket_impl_;
};

/// Free functions for full read/write operations

/// Read exactly n bytes
inline auto async_read(tcp_socket& s, std::span<char> buffer, std::chrono::milliseconds timeout = {}) -> awaitable<void> {
  std::size_t total = 0;
  while (total < buffer.size()) {
    auto n = co_await s.async_read_some(buffer.subspan(total), timeout);
    if (n == 0) throw std::system_error(error::eof);
    total += n;
  }
}

/// Write all data
inline auto async_write(tcp_socket& s, std::span<char const> buffer, std::chrono::milliseconds timeout = {})
    -> awaitable<void> {
  std::size_t total = 0;
  while (total < buffer.size()) {
    auto n = co_await s.async_write_some(buffer.subspan(total), timeout);
    total += n;
  }
}

}  // namespace xz::io

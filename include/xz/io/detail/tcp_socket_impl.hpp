#pragma once

#include <xz/io/io_context.hpp>
#include <xz/io/ip.hpp>
#include <xz/io/tcp_socket.hpp>

namespace xz::io::detail {

class tcp_socket_impl {
 public:
  explicit tcp_socket_impl(io_context& ctx) : ctx_(ctx) {}

  ~tcp_socket_impl() { close(); }

  auto get_executor() noexcept -> io_context& { return ctx_; }

  auto is_open() const noexcept -> bool { return fd_ >= 0; }

  auto native_handle() const noexcept -> int { return fd_; }

  void close();
  void close(std::error_code& ec) noexcept;

  void set_option_nodelay(bool enable);
  void set_option_keepalive(bool enable);
  void set_option_reuseaddr(bool enable);

  auto local_endpoint() const -> ip::tcp_endpoint;
  auto remote_endpoint() const -> ip::tcp_endpoint;

  // Internal methods for async operations
  auto connect(ip::tcp_endpoint const& ep, std::chrono::milliseconds timeout) -> std::error_code;
  auto read_some(std::span<char> buffer) -> std::pair<std::error_code, std::size_t>;
  auto write_some(std::span<char const> buffer) -> std::pair<std::error_code, std::size_t>;

 private:
  auto create_and_connect(ip::tcp_endpoint const& ep) -> std::error_code;
  auto set_nonblocking() -> std::error_code;

  io_context& ctx_;
  int fd_ = -1;
};

}  // namespace xz::io::detail

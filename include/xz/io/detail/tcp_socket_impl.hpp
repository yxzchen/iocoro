#pragma once

#include <xz/io/expected.hpp>
#include <xz/io/io_context.hpp>
#include <xz/io/ip.hpp>

namespace xz::io::detail {

class tcp_socket_impl {
 public:
  explicit tcp_socket_impl(io_context& ctx) : ctx_(ctx) {}

  ~tcp_socket_impl() { close(); }

  auto get_executor() noexcept -> io_context& { return ctx_; }

  auto is_open() const noexcept -> bool { return fd_ >= 0; }

  auto native_handle() const noexcept -> int { return fd_; }

  void close();
  auto close_nothrow() noexcept -> std::error_code;

  auto set_option_nodelay(bool enable) -> std::error_code;
  auto set_option_keepalive(bool enable) -> std::error_code;
  auto set_option_reuseaddr(bool enable) -> std::error_code;

  auto local_endpoint() const -> expected<ip::tcp_endpoint, std::error_code>;
  auto remote_endpoint() const -> expected<ip::tcp_endpoint, std::error_code>;

  // Internal methods for async operations
  auto connect(ip::tcp_endpoint const& ep) -> std::error_code;
  auto read_some(std::span<char> buffer) -> expected<std::size_t, std::error_code>;
  auto write_some(std::span<char const> buffer) -> expected<std::size_t, std::error_code>;

 private:
  auto create_and_connect(ip::tcp_endpoint const& ep) -> std::error_code;
  auto set_nonblocking() -> std::error_code;

  io_context& ctx_;
  int fd_ = -1;
};

}  // namespace xz::io::detail

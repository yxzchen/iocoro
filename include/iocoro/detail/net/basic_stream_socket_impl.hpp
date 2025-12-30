#pragma once

#include <iocoro/awaitable.hpp>
#include <iocoro/error.hpp>
#include <iocoro/io_executor.hpp>
#include <iocoro/expected.hpp>
#include <iocoro/shutdown.hpp>

#include <iocoro/detail/socket/stream_socket_impl.hpp>

#include <cstddef>
#include <span>
#include <system_error>

// Native socket APIs (generic / non-domain-specific).
#include <sys/socket.h>
#include <cerrno>

namespace iocoro::detail::net {

/// Generic stream-socket implementation for sockaddr-based protocols, parameterized by Protocol.
///
/// Boundary:
/// - Depends on `Protocol::type()` / `Protocol::protocol()` only for socket creation.
/// - Endpoint semantics are NOT interpreted here; endpoint is a native view with `family()`.
template <class Protocol>
class basic_stream_socket_impl {
 public:
  using endpoint_type = typename Protocol::endpoint;

  basic_stream_socket_impl() noexcept = delete;
  explicit basic_stream_socket_impl(io_executor ex) noexcept : stream_(ex) {}

  basic_stream_socket_impl(basic_stream_socket_impl const&) = delete;
  auto operator=(basic_stream_socket_impl const&) -> basic_stream_socket_impl& = delete;
  basic_stream_socket_impl(basic_stream_socket_impl&&) = delete;
  auto operator=(basic_stream_socket_impl&&) -> basic_stream_socket_impl& = delete;

  ~basic_stream_socket_impl() = default;

  auto get_executor() const noexcept -> io_executor { return stream_.get_executor(); }
  auto native_handle() const noexcept -> int { return stream_.native_handle(); }
  auto is_open() const noexcept -> bool { return stream_.is_open(); }

  void cancel() noexcept { stream_.cancel(); }
  void cancel_read() noexcept { stream_.cancel_read(); }
  void cancel_write() noexcept { stream_.cancel_write(); }
  void close() noexcept { stream_.close(); }

  auto local_endpoint() const -> expected<endpoint_type, std::error_code>;

  auto remote_endpoint() const -> expected<endpoint_type, std::error_code>;

  auto shutdown(shutdown_type what) -> std::error_code { return stream_.shutdown(what); }

  auto is_connected() const noexcept -> bool { return stream_.is_connected(); }

  template <class Option>
  auto set_option(Option const& opt) -> std::error_code {
    return stream_.set_option(opt);
  }

  template <class Option>
  auto get_option(Option& opt) -> std::error_code {
    return stream_.get_option(opt);
  }

  auto async_connect(endpoint_type const& ep) -> awaitable<std::error_code>;

  auto async_read_some(std::span<std::byte> buffer)
    -> awaitable<expected<std::size_t, std::error_code>> {
    return stream_.async_read_some(buffer);
  }

  auto async_write_some(std::span<std::byte const> buffer)
    -> awaitable<expected<std::size_t, std::error_code>> {
    return stream_.async_write_some(buffer);
  }

  /// Adopt an already-connected native fd (from accept()).
  auto assign(int fd) noexcept -> std::error_code { return stream_.assign(fd); }

 private:
  socket::stream_socket_impl stream_;
};

}  // namespace iocoro::detail::net

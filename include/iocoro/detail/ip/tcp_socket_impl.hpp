#pragma once

#include <iocoro/awaitable.hpp>
#include <iocoro/error.hpp>
#include <iocoro/executor.hpp>
#include <iocoro/expected.hpp>
#include <iocoro/shutdown.hpp>

#include <iocoro/detail/socket/stream_socket_impl.hpp>
#include <iocoro/ip/endpoint.hpp>

#include <cstddef>
#include <span>
#include <system_error>

namespace iocoro::detail::ip {

/// TCP socket implementation (IP-specific adapter).
///
/// Design choice:
/// - Uses composition (NOT inheritance): holds a `socket::stream_socket_impl`.
/// - This avoids exposing unrelated stream interfaces when we add other stream protocols
///   (e.g. Unix domain sockets) that also reuse `stream_socket_impl`.
class tcp_socket_impl {
 public:
  tcp_socket_impl() noexcept = default;
  explicit tcp_socket_impl(executor ex) noexcept : stream_(ex) {}

  tcp_socket_impl(tcp_socket_impl const&) = delete;
  auto operator=(tcp_socket_impl const&) -> tcp_socket_impl& = delete;
  tcp_socket_impl(tcp_socket_impl&&) = delete;
  auto operator=(tcp_socket_impl&&) -> tcp_socket_impl& = delete;

  ~tcp_socket_impl() = default;

  auto get_executor() const noexcept -> executor { return stream_.get_executor(); }
  auto native_handle() const noexcept -> int { return stream_.native_handle(); }
  auto is_open() const noexcept -> bool { return stream_.is_open(); }

  void cancel() noexcept { stream_.cancel(); }
  void cancel_read() noexcept { stream_.cancel_read(); }
  void cancel_write() noexcept { stream_.cancel_write(); }
  void close() noexcept { stream_.close(); }

  auto local_endpoint() const -> iocoro::ip::endpoint { return iocoro::ip::endpoint{}; }
  auto remote_endpoint() const -> iocoro::ip::endpoint { return iocoro::ip::endpoint{}; }

  auto shutdown(shutdown_type what) -> std::error_code { return stream_.shutdown(what); }

  auto is_connected() const noexcept -> bool { return false; }

  template <class Option>
  auto set_option(Option const& opt) -> std::error_code {
    return stream_.set_option(opt);
  }

  template <class Option>
  auto get_option(Option& opt) -> std::error_code {
    return stream_.get_option(opt);
  }

  auto async_connect(iocoro::ip::endpoint const& ep) -> awaitable<std::error_code> {
    return stream_.async_connect(ep.data(), ep.size());
  }

  auto async_read_some(std::span<std::byte> buffer)
    -> awaitable<expected<std::size_t, std::error_code>> {
    return stream_.async_read_some(buffer);
  }

  auto async_write_some(std::span<std::byte const> buffer)
    -> awaitable<expected<std::size_t, std::error_code>> {
    return stream_.async_write_some(buffer);
  }

 private:
  socket::stream_socket_impl stream_{};
};

}  // namespace iocoro::detail::ip

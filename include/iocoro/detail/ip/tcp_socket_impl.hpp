#pragma once

#include <iocoro/awaitable.hpp>
#include <iocoro/error.hpp>
#include <iocoro/executor.hpp>
#include <iocoro/expected.hpp>
#include <iocoro/use_awaitable.hpp>

#include <iocoro/detail/socket/stream_socket_impl.hpp>
#include <iocoro/ip/endpoint.hpp>

#include <cstddef>
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
  void close() noexcept { stream_.close(); }

  auto async_connect(use_awaitable_t t, iocoro::ip::endpoint const& ep)
    -> awaitable<std::error_code> {
    (void)t;
    return stream_.async_connect(use_awaitable, ep.data(), ep.size());
  }

  auto async_read_some(use_awaitable_t t, void* data, std::size_t size)
    -> awaitable<expected<std::size_t, std::error_code>> {
    (void)t;
    return stream_.async_read_some(use_awaitable, data, size);
  }

  auto async_write_some(use_awaitable_t t, void const* data, std::size_t size)
    -> awaitable<expected<std::size_t, std::error_code>> {
    (void)t;
    return stream_.async_write_some(use_awaitable, data, size);
  }

 private:
  socket::stream_socket_impl stream_{};
};

}  // namespace iocoro::detail::ip

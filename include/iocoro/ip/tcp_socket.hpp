#pragma once

#include <iocoro/basic_socket.hpp>
#include <iocoro/detail/ip/tcp_socket_impl.hpp>
#include <iocoro/ip/endpoint.hpp>

#include <iocoro/awaitable.hpp>
#include <iocoro/expected.hpp>
#include <iocoro/use_awaitable.hpp>

#include <cstddef>
#include <memory>
#include <system_error>

namespace iocoro::ip {

using tcp_socket_impl = ::iocoro::detail::ip::tcp_socket_impl;

/// Public TCP socket type (RAII + coroutine async interface).
///
/// First-stage contract:
/// - Only `use_awaitable` based async APIs are provided.
/// - Implementations are stubs for now (compilation-only).
/// - Future: methods will perform real non-blocking I/O backed by io_context_impl.
class tcp_socket : public basic_socket<tcp_socket_impl> {
 public:
  using base_type = basic_socket<tcp_socket_impl>;

  tcp_socket() noexcept = default;

  explicit tcp_socket(executor ex);
  explicit tcp_socket(io_context& ctx);

  auto async_connect(use_awaitable_t, endpoint const& ep) -> awaitable<std::error_code>;

  auto async_read_some(use_awaitable_t, void* data, std::size_t size)
    -> awaitable<expected<std::size_t, std::error_code>>;

  auto async_write_some(use_awaitable_t, void const* data, std::size_t size)
    -> awaitable<expected<std::size_t, std::error_code>>;
};

}  // namespace iocoro::ip

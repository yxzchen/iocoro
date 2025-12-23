#pragma once

#include <iocoro/basic_socket.hpp>
#include <iocoro/detail/ip/tcp_socket_impl.hpp>
#include <iocoro/ip/endpoint.hpp>

#include <iocoro/awaitable.hpp>
#include <iocoro/expected.hpp>
#include <iocoro/shutdown.hpp>
#include <iocoro/use_awaitable.hpp>

#include <cstddef>
#include <memory>
#include <span>
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

  tcp_socket(tcp_socket const&) = delete;
  auto operator=(tcp_socket const&) -> tcp_socket& = delete;

  /// Move assignment.
  /// Note: if the moved-from socket had pending operations, they may continue to run
  /// against the moved-from object's impl instance (impl is shared_ptr-based).
  tcp_socket(tcp_socket&&) = default;
  auto operator=(tcp_socket&&) -> tcp_socket& = default;

  auto async_connect(use_awaitable_t, endpoint const& ep) -> awaitable<std::error_code>;

  auto async_read_some(use_awaitable_t, std::span<std::byte> buffer)
    -> awaitable<expected<std::size_t, std::error_code>>;

  auto async_write_some(use_awaitable_t, std::span<std::byte const> buffer)
    -> awaitable<expected<std::size_t, std::error_code>>;

  auto local_endpoint() const -> endpoint;
  auto remote_endpoint() const -> endpoint;

  auto shutdown(shutdown_type what) -> std::error_code;

  auto is_connected() const noexcept -> bool;

  using base_type::cancel;
  using base_type::close;
  using base_type::get_option;
  using base_type::is_open;
  using base_type::native_handle;
  using base_type::set_option;
};

}  // namespace iocoro::ip

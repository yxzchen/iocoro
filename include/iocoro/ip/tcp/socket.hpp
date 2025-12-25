#pragma once

#include <iocoro/awaitable.hpp>
#include <iocoro/basic_socket.hpp>
#include <iocoro/expected.hpp>
#include <iocoro/ip/tcp/endpoint.hpp>
#include <iocoro/shutdown.hpp>

#include <cstddef>
#include <memory>
#include <span>
#include <system_error>

namespace iocoro::detail::ip::tcp {
class socket_impl;
}

namespace iocoro::ip::tcp {

/// Public TCP socket type (RAII + coroutine async interface).
///
/// First-stage contract:
/// - Only coroutine-based async APIs are provided (no completion tokens).
/// - Implementations are stubs for now (compilation-only).
/// - Future: methods will perform real non-blocking I/O backed by io_context_impl.
class socket : public basic_socket<detail::ip::tcp::socket_impl> {
 public:
  using base_type = basic_socket<detail::ip::tcp::socket_impl>;

  socket() = delete;

  explicit socket(executor ex);
  explicit socket(io_context& ctx);

  socket(socket const&) = delete;
  auto operator=(socket const&) -> socket& = delete;

  /// Move assignment.
  /// Note: if the moved-from socket had pending operations, they may continue to run
  /// against the moved-from object's impl instance (impl is shared_ptr-based).
  socket(socket&&) = default;
  auto operator=(socket&&) -> socket& = default;

  auto async_connect(endpoint const& ep) -> awaitable<std::error_code>;

  auto async_read_some(std::span<std::byte> buffer)
    -> awaitable<expected<std::size_t, std::error_code>>;

  auto async_write_some(std::span<std::byte const> buffer)
    -> awaitable<expected<std::size_t, std::error_code>>;

  auto local_endpoint() const -> expected<endpoint, std::error_code>;
  auto remote_endpoint() const -> expected<endpoint, std::error_code>;

  auto shutdown(shutdown_type what) -> std::error_code;

  auto is_connected() const noexcept -> bool;

  using base_type::get_executor;
  using base_type::native_handle;

  using base_type::close;
  using base_type::is_open;

  using base_type::cancel;
  using base_type::cancel_read;
  using base_type::cancel_write;

  using base_type::get_option;
  using base_type::set_option;
};

}  // namespace iocoro::ip::tcp

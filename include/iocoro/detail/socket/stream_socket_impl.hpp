#pragma once

#include <iocoro/awaitable.hpp>
#include <iocoro/error.hpp>
#include <iocoro/expected.hpp>
#include <iocoro/shutdown.hpp>
#include <iocoro/use_awaitable.hpp>

#include <iocoro/detail/socket/socket_impl_base.hpp>

#include <cstddef>
#include <span>
#include <system_error>

// Native socket address types (POSIX).
#include <sys/socket.h>

namespace iocoro::detail::socket {

/// Stream-socket implementation shared by multiple protocols.
///
/// This layer does NOT know about ip::endpoint (or any higher-level endpoint type).
/// It accepts native `(sockaddr*, socklen_t)` views.
///
/// Concurrency:
/// - At most one in-flight read and one in-flight write are intended (full-duplex).
/// - Conflicting operations should return `error::busy` (first stage: stub).
class stream_socket_impl {
 public:
  stream_socket_impl() noexcept = default;
  explicit stream_socket_impl(executor ex) noexcept : base_(ex) {}

  stream_socket_impl(stream_socket_impl const&) = delete;
  auto operator=(stream_socket_impl const&) -> stream_socket_impl& = delete;
  stream_socket_impl(stream_socket_impl&&) = delete;
  auto operator=(stream_socket_impl&&) -> stream_socket_impl& = delete;

  ~stream_socket_impl() = default;

  auto get_executor() const noexcept -> executor { return base_.get_executor(); }
  auto native_handle() const noexcept -> int { return base_.native_handle(); }
  auto is_open() const noexcept -> bool { return base_.is_open(); }

  void cancel() noexcept { base_.cancel(); }
  void close() noexcept { base_.close(); }

  template <class Option>
  auto set_option(Option const& opt) -> std::error_code {
    return base_.set_option(opt);
  }

  template <class Option>
  auto get_option(Option& opt) -> std::error_code {
    return base_.get_option(opt);
  }

  /// Connect to a native endpoint.
  auto async_connect(use_awaitable_t, sockaddr const* /*addr*/, socklen_t /*len*/)
    -> awaitable<std::error_code> {
    co_return error::not_implemented;
  }

  /// Read at most `size` bytes into `data`.
  auto async_read_some(use_awaitable_t, std::span<std::byte> /*buffer*/)
    -> awaitable<expected<std::size_t, std::error_code>> {
    co_return unexpected<std::error_code>(error::not_implemented);
  }

  /// Write at most `size` bytes from `data`.
  auto async_write_some(use_awaitable_t, std::span<std::byte const> /*buffer*/)
    -> awaitable<expected<std::size_t, std::error_code>> {
    co_return unexpected<std::error_code>(error::not_implemented);
  }

  auto shutdown_read() noexcept -> std::error_code { return error::not_implemented; }
  auto shutdown_write() noexcept -> std::error_code { return error::not_implemented; }
  auto shutdown_both() noexcept -> std::error_code { return error::not_implemented; }

  auto shutdown(shutdown_type /*what*/) -> std::error_code { return error::not_implemented; }

 private:
  socket_impl_base base_{};
};

}  // namespace iocoro::detail::socket

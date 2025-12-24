#include <iocoro/detail/ip/tcp_socket_impl.hpp>
#include <iocoro/ip/tcp_socket.hpp>

#include <iocoro/error.hpp>

namespace iocoro::ip {

inline tcp_socket::tcp_socket(executor ex) : base_type(ex) {}

inline tcp_socket::tcp_socket(io_context& ctx) : base_type(ctx) {}

inline auto tcp_socket::async_connect(endpoint const& ep) -> awaitable<std::error_code> {
  co_return co_await impl_->async_connect(ep);
}

inline auto tcp_socket::async_read_some(std::span<std::byte> buffer)
  -> awaitable<expected<std::size_t, std::error_code>> {
  co_return co_await impl_->async_read_some(buffer);
}

inline auto tcp_socket::async_write_some(std::span<std::byte const> buffer)
  -> awaitable<expected<std::size_t, std::error_code>> {
  co_return co_await impl_->async_write_some(buffer);
}

inline auto tcp_socket::local_endpoint() const -> expected<endpoint, std::error_code> {
  return impl_->local_endpoint();
}

inline auto tcp_socket::remote_endpoint() const -> expected<endpoint, std::error_code> {
  return impl_->remote_endpoint();
}

inline auto tcp_socket::shutdown(shutdown_type what) -> std::error_code {
  return impl_->shutdown(what);
}

inline auto tcp_socket::is_connected() const noexcept -> bool { return impl_->is_connected(); }

}  // namespace iocoro::ip

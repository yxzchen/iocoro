#include <iocoro/detail/ip/tcp/socket_impl.hpp>
#include <iocoro/ip/tcp/socket.hpp>

#include <iocoro/error.hpp>

namespace iocoro::ip::tcp {

inline socket::socket(executor ex) : base_type(ex) {}

inline socket::socket(io_context& ctx) : base_type(ctx) {}

inline auto socket::async_connect(endpoint const& ep) -> awaitable<std::error_code> {
  return impl_->async_connect(ep);
}

inline auto socket::async_read_some(std::span<std::byte> buffer)
  -> awaitable<expected<std::size_t, std::error_code>> {
  return impl_->async_read_some(buffer);
}

inline auto socket::async_write_some(std::span<std::byte const> buffer)
  -> awaitable<expected<std::size_t, std::error_code>> {
  return impl_->async_write_some(buffer);
}

inline auto socket::local_endpoint() const -> expected<endpoint, std::error_code> {
  return impl_->local_endpoint();
}

inline auto socket::remote_endpoint() const -> expected<endpoint, std::error_code> {
  return impl_->remote_endpoint();
}

inline auto socket::shutdown(shutdown_type what) -> std::error_code {
  return impl_->shutdown(what);
}

inline auto socket::is_connected() const noexcept -> bool { return impl_->is_connected(); }

}  // namespace iocoro::ip::tcp



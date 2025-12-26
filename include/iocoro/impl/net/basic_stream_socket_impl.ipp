#include <iocoro/detail/net/basic_stream_socket_impl.hpp>

namespace iocoro::detail::net {

template <class Protocol>
auto basic_stream_socket_impl<Protocol>::local_endpoint() const
  -> expected<endpoint_type, std::error_code> {
  auto const fd = stream_.native_handle();
  if (fd < 0) {
    return unexpected(error::not_open);
  }

  sockaddr_storage ss{};
  socklen_t len = sizeof(ss);
  if (::getsockname(fd, reinterpret_cast<sockaddr*>(&ss), &len) != 0) {
    return unexpected(std::error_code(errno, std::generic_category()));
  }
  return endpoint_type::from_native(reinterpret_cast<sockaddr*>(&ss), len);
}

template <class Protocol>
auto basic_stream_socket_impl<Protocol>::remote_endpoint() const
  -> expected<endpoint_type, std::error_code> {
  auto const fd = stream_.native_handle();
  if (fd < 0) {
    return unexpected(error::not_open);
  }
  if (!stream_.is_connected()) {
    return unexpected(error::not_connected);
  }

  sockaddr_storage ss{};
  socklen_t len = sizeof(ss);
  if (::getpeername(fd, reinterpret_cast<sockaddr*>(&ss), &len) != 0) {
    if (errno == ENOTCONN) {
      return unexpected(error::not_connected);
    }
    return unexpected(std::error_code(errno, std::generic_category()));
  }
  return endpoint_type::from_native(reinterpret_cast<sockaddr*>(&ss), len);
}

template <class Protocol>
auto basic_stream_socket_impl<Protocol>::async_connect(endpoint_type const& ep)
  -> awaitable<std::error_code> {
  // Lazy-open based on endpoint family; protocol specifics come from Protocol tag.
  if (!stream_.is_open()) {
    auto ec = stream_.open(ep.family(), Protocol::type(), Protocol::protocol());
    if (ec) {
      co_return ec;
    }
  }
  co_return co_await stream_.async_connect(ep.data(), ep.size());
}

}  // namespace iocoro::detail::net



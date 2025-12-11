
#include <xz/io/detail/tcp_socket_impl.hpp>
#include <xz/io/error.hpp>

#include <cerrno>
#include <cstring>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

namespace xz::io {

/// Template implementation of async_io_operation methods
template <typename Result>
void async_io_operation<Result>::setup_timeout() {
  if (timeout_.count() > 0) {
    auto socket_impl = get_socket_impl();
    if (!socket_impl) return;

    timer_handle_ = socket_impl->get_executor().schedule_timer(
        timeout_,
        [this, weak_socket_impl = socket_impl_]() {
          auto socket_impl = weak_socket_impl.lock();
          if (!socket_impl) {
            this->complete(make_error_code(error::operation_aborted));
            return;
          }

          socket_impl->get_executor().deregister_fd(socket_impl->native_handle());
          timer_handle_.reset();
          if constexpr (std::is_void_v<Result>) {
            this->complete(make_error_code(error::timeout));
          } else {
            this->complete(make_error_code(error::timeout), Result{});
          }
        });
  }
}

template <typename Result>
void async_io_operation<Result>::cleanup_timer() {
  if (timer_handle_) {
    auto socket_impl = get_socket_impl();
    if (socket_impl) {
      socket_impl->get_executor().cancel_timer(timer_handle_);
    }
    timer_handle_.reset();
  }
}

}  // namespace xz::io

namespace xz::io::detail {

void tcp_socket_impl::close() {
  if (fd_ >= 0) {
    ctx_.deregister_fd(fd_);
    ::close(fd_);
    fd_ = -1;
  }
}

auto tcp_socket_impl::close_nothrow() noexcept -> expected<void, std::error_code> {
  try {
    close();
    return {};
  } catch (std::system_error const& e) {
    return unexpected(e.code());
  }
}

auto tcp_socket_impl::set_nonblocking() -> std::error_code {
  int flags = ::fcntl(fd_, F_GETFL, 0);
  if (flags < 0) {
    return std::error_code(errno, std::generic_category());
  }

  if (::fcntl(fd_, F_SETFL, flags | O_NONBLOCK) < 0) {
    return std::error_code(errno, std::generic_category());
  }

  return {};
}

auto tcp_socket_impl::create_and_connect(ip::tcp_endpoint const& ep) -> std::error_code {
  int family = ep.is_v6() ? AF_INET6 : AF_INET;
  fd_ = ::socket(family, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd_ < 0) {
    return std::error_code(errno, std::generic_category());
  }

  if (auto ec = set_nonblocking(); ec) {
    ::close(fd_);
    fd_ = -1;
    return ec;
  }

  if (ep.is_v6()) {
    sockaddr_in6 addr{};
    addr.sin6_family = AF_INET6;
    addr.sin6_port = htons(ep.port());
    auto bytes = ep.get_address_v6().to_bytes();
    std::memcpy(&addr.sin6_addr, bytes.data(), 16);

    int res = ::connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if (res < 0 && errno != EINPROGRESS) {
      auto err = errno;
      ::close(fd_);
      fd_ = -1;
      return std::error_code(err, std::generic_category());
    }
  } else {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(ep.port());
    addr.sin_addr.s_addr = htonl(ep.get_address_v4().to_uint());

    int res = ::connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if (res < 0 && errno != EINPROGRESS) {
      auto err = errno;
      ::close(fd_);
      fd_ = -1;
      return std::error_code(err, std::generic_category());
    }
  }

  return {};
}

auto tcp_socket_impl::connect(ip::tcp_endpoint const& ep) -> std::error_code {
  if (is_open()) {
    return make_error_code(error::already_connected);
  }

  auto ec = create_and_connect(ep);
  if (ec) {
    return ec;
  }
  return {};
}

auto tcp_socket_impl::read_some(std::span<char> buffer) -> expected<std::size_t, std::error_code> {
  if (!is_open()) {
    return unexpected(make_error_code(error::not_connected));
  }

  ssize_t n = ::recv(fd_, buffer.data(), buffer.size(), 0);

  if (n < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return unexpected(std::make_error_code(std::errc::operation_would_block));
    }
    return unexpected(std::error_code(errno, std::generic_category()));
  }

  if (n == 0) {
    return unexpected(make_error_code(error::eof));
  }

  return static_cast<std::size_t>(n);
}

auto tcp_socket_impl::write_some(std::span<char const> buffer) -> expected<std::size_t, std::error_code> {
  if (!is_open()) {
    return unexpected(make_error_code(error::not_connected));
  }

  ssize_t n = ::send(fd_, buffer.data(), buffer.size(), MSG_NOSIGNAL);

  if (n < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return unexpected(std::make_error_code(std::errc::operation_would_block));
    }
    return unexpected(std::error_code(errno, std::generic_category()));
  }

  return static_cast<std::size_t>(n);
}

auto tcp_socket_impl::set_option_nodelay(bool enable) -> expected<void, std::error_code> {
  if (!is_open()) {
    return unexpected(make_error_code(error::not_connected));
  }

  int flag = enable ? 1 : 0;
  if (::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag)) < 0) {
    return unexpected(std::error_code(errno, std::generic_category()));
  }
  return {};
}

auto tcp_socket_impl::set_option_keepalive(bool enable) -> expected<void, std::error_code> {
  if (!is_open()) {
    return unexpected(make_error_code(error::not_connected));
  }

  int flag = enable ? 1 : 0;
  if (::setsockopt(fd_, SOL_SOCKET, SO_KEEPALIVE, &flag, sizeof(flag)) < 0) {
    return unexpected(std::error_code(errno, std::generic_category()));
  }
  return {};
}

auto tcp_socket_impl::set_option_reuseaddr(bool enable) -> expected<void, std::error_code> {
  if (!is_open()) {
    return unexpected(make_error_code(error::not_connected));
  }

  int flag = enable ? 1 : 0;
  if (::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag)) < 0) {
    return unexpected(std::error_code(errno, std::generic_category()));
  }
  return {};
}

namespace {

auto sockaddr_to_endpoint(sockaddr_storage const& addr) -> ip::tcp_endpoint {
  if (addr.ss_family == AF_INET) {
    auto* in = reinterpret_cast<sockaddr_in const*>(&addr);
    return {ip::address_v4{ntohl(in->sin_addr.s_addr)}, ntohs(in->sin_port)};
  } else if (addr.ss_family == AF_INET6) {
    auto* in6 = reinterpret_cast<sockaddr_in6 const*>(&addr);
    ip::address_v6::bytes_type bytes;
    std::memcpy(bytes.data(), &in6->sin6_addr, 16);
    return {ip::address_v6{bytes}, ntohs(in6->sin6_port)};
  }
  throw std::system_error(make_error_code(error::invalid_argument));
}

}  // namespace

auto tcp_socket_impl::local_endpoint() const -> expected<ip::tcp_endpoint, std::error_code> {
  if (!is_open()) {
    return unexpected(make_error_code(error::not_connected));
  }

  sockaddr_storage addr{};
  socklen_t len = sizeof(addr);

  if (::getsockname(fd_, reinterpret_cast<sockaddr*>(&addr), &len) < 0) {
    return unexpected(std::error_code(errno, std::generic_category()));
  }

  try {
    return sockaddr_to_endpoint(addr);
  } catch (...) {
    return unexpected(make_error_code(error::invalid_argument));
  }
}

auto tcp_socket_impl::remote_endpoint() const -> expected<ip::tcp_endpoint, std::error_code> {
  if (!is_open()) {
    return unexpected(make_error_code(error::not_connected));
  }

  sockaddr_storage addr{};
  socklen_t len = sizeof(addr);

  if (::getpeername(fd_, reinterpret_cast<sockaddr*>(&addr), &len) < 0) {
    return unexpected(std::error_code(errno, std::generic_category()));
  }

  try {
    return sockaddr_to_endpoint(addr);
  } catch (...) {
    return unexpected(make_error_code(error::invalid_argument));
  }
}

}  // namespace xz::io::detail

namespace xz::io {

tcp_socket::tcp_socket(io_context& ctx) : socket_impl_(std::make_shared<detail::tcp_socket_impl>(ctx)) {}

tcp_socket::~tcp_socket() = default;

tcp_socket::tcp_socket(tcp_socket&&) noexcept = default;

auto tcp_socket::operator=(tcp_socket&&) noexcept -> tcp_socket& = default;

auto tcp_socket::get_executor() noexcept -> io_context& { return socket_impl_->get_executor(); }

auto tcp_socket::is_open() const noexcept -> bool { return socket_impl_->is_open(); }

auto tcp_socket::native_handle() const noexcept -> int { return socket_impl_->native_handle(); }

void tcp_socket::close() { socket_impl_->close(); }

auto tcp_socket::close_nothrow() noexcept -> expected<void, std::error_code> {
  return socket_impl_->close_nothrow();
}

auto tcp_socket::set_option_nodelay(bool enable) -> expected<void, std::error_code> {
  return socket_impl_->set_option_nodelay(enable);
}

auto tcp_socket::set_option_keepalive(bool enable) -> expected<void, std::error_code> {
  return socket_impl_->set_option_keepalive(enable);
}

auto tcp_socket::set_option_reuseaddr(bool enable) -> expected<void, std::error_code> {
  return socket_impl_->set_option_reuseaddr(enable);
}

auto tcp_socket::local_endpoint() const -> expected<ip::tcp_endpoint, std::error_code> {
  return socket_impl_->local_endpoint();
}

auto tcp_socket::remote_endpoint() const -> expected<ip::tcp_endpoint, std::error_code> {
  return socket_impl_->remote_endpoint();
}

tcp_socket::async_connect_op::async_connect_op(std::weak_ptr<detail::tcp_socket_impl> socket_impl,
                                                ip::tcp_endpoint ep,
                                                std::chrono::milliseconds timeout)
    : async_io_operation<void>(std::move(socket_impl), timeout), endpoint_(ep) {}

void tcp_socket::async_connect_op::start_operation() {
  auto socket_impl = get_socket_impl();
  if (!socket_impl) {
    complete(make_error_code(error::operation_aborted));
    return;
  }

  auto ec = socket_impl->connect(endpoint_);
  if (ec) {
    if (ec != std::errc::operation_in_progress) {
      complete(ec);
      return;
    }
  }

  setup_timeout();

  struct connect_operation : io_context::operation_base {
    std::weak_ptr<detail::tcp_socket_impl> socket_impl;
    tcp_socket::async_connect_op* op;

    connect_operation(std::weak_ptr<detail::tcp_socket_impl> s, tcp_socket::async_connect_op* o)
        : socket_impl(std::move(s)), op(o) {}

    void execute() override {
      auto socket_impl_ptr = socket_impl.lock();
      if (!socket_impl_ptr) {
        op->complete(make_error_code(error::operation_aborted));
        return;
      }

      int error = 0;
      socklen_t len = sizeof(error);
      ::getsockopt(socket_impl_ptr->native_handle(), SOL_SOCKET, SO_ERROR, &error, &len);

      socket_impl_ptr->get_executor().deregister_fd(socket_impl_ptr->native_handle());
      op->cleanup_timer();

      if (error) {
        op->complete(std::error_code(error, std::generic_category()));
      } else {
        op->complete({});
      }
    }
  };

  socket_impl->get_executor().register_fd_write(
      socket_impl->native_handle(),
      std::make_unique<connect_operation>(socket_impl_, this));
}

tcp_socket::async_read_some_op::async_read_some_op(std::weak_ptr<detail::tcp_socket_impl> socket_impl,
                                                    std::span<char> buf,
                                                    std::chrono::milliseconds timeout)
    : async_io_operation<std::size_t>(std::move(socket_impl), timeout), buffer_(buf) {}

void tcp_socket::async_read_some_op::start_operation() {
  auto socket_impl = get_socket_impl();
  if (!socket_impl) {
    complete(make_error_code(error::operation_aborted));
    return;
  }

  auto result = socket_impl->read_some(buffer_);
  if (result) {
    complete({}, *result);
    return;
  }

  auto ec = result.error();
  if (ec != std::errc::operation_would_block) {
    complete(ec);
    return;
  }

  setup_timeout();

  struct read_operation : io_context::operation_base {
    std::weak_ptr<detail::tcp_socket_impl> socket_impl;
    std::span<char> buffer;
    tcp_socket::async_read_some_op* op;

    read_operation(std::weak_ptr<detail::tcp_socket_impl> s, std::span<char> buf,
                   tcp_socket::async_read_some_op* o)
        : socket_impl(std::move(s)), buffer(buf), op(o) {}

    void execute() override {
      auto socket_impl_ptr = socket_impl.lock();
      if (!socket_impl_ptr) {
        op->complete(make_error_code(error::operation_aborted));
        return;
      }

      auto result = socket_impl_ptr->read_some(buffer);

      if (!result && result.error() == std::errc::operation_would_block) {
        socket_impl_ptr->get_executor().register_fd_read(
            socket_impl_ptr->native_handle(),
            std::make_unique<read_operation>(socket_impl, buffer, op));
        return;
      }

      op->cleanup_timer();

      if (result) {
        op->complete({}, *result);
      } else {
        op->complete(result.error());
      }
    }
  };

  socket_impl->get_executor().register_fd_read(
      socket_impl->native_handle(),
      std::make_unique<read_operation>(socket_impl_, buffer_, this));
}

tcp_socket::async_write_some_op::async_write_some_op(std::weak_ptr<detail::tcp_socket_impl> socket_impl,
                                                      std::span<char const> buf,
                                                      std::chrono::milliseconds timeout)
    : async_io_operation<std::size_t>(std::move(socket_impl), timeout), buffer_(buf) {}

void tcp_socket::async_write_some_op::start_operation() {
  auto socket_impl = get_socket_impl();
  if (!socket_impl) {
    complete(make_error_code(error::operation_aborted));
    return;
  }

  auto result = socket_impl->write_some(buffer_);
  if (result) {
    complete({}, *result);
    return;
  }

  auto ec = result.error();
  if (ec != std::errc::operation_would_block) {
    complete(ec);
    return;
  }

  setup_timeout();

  struct write_operation : io_context::operation_base {
    std::weak_ptr<detail::tcp_socket_impl> socket_impl;
    std::span<char const> buffer;
    tcp_socket::async_write_some_op* op;

    write_operation(std::weak_ptr<detail::tcp_socket_impl> s, std::span<char const> buf,
                    tcp_socket::async_write_some_op* o)
        : socket_impl(std::move(s)), buffer(buf), op(o) {}

    void execute() override {
      auto socket_impl_ptr = socket_impl.lock();
      if (!socket_impl_ptr) {
        op->complete(make_error_code(error::operation_aborted));
        return;
      }

      auto result = socket_impl_ptr->write_some(buffer);

      if (!result && result.error() == std::errc::operation_would_block) {
        socket_impl_ptr->get_executor().register_fd_write(
            socket_impl_ptr->native_handle(),
            std::make_unique<write_operation>(socket_impl, buffer, op));
        return;
      }

      op->cleanup_timer();

      if (result) {
        op->complete({}, *result);
      } else {
        op->complete(result.error());
      }
    }
  };

  socket_impl->get_executor().register_fd_write(
      socket_impl->native_handle(),
      std::make_unique<write_operation>(socket_impl_, buffer_, this));
}

}  // namespace xz::io

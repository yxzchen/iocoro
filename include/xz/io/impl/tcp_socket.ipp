#pragma once

#include <xz/io/detail/tcp_socket_impl.hpp>
#include <xz/io/error.hpp>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace xz::io::detail {

void tcp_socket_impl::close() {
  if (fd_ >= 0) {
    ctx_.deregister_fd(fd_);
    ::close(fd_);
    fd_ = -1;
  }
}

void tcp_socket_impl::close(std::error_code& ec) noexcept {
  ec.clear();
  try {
    close();
  } catch (std::system_error const& e) {
    ec = e.code();
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
  // Create socket
  int family = ep.is_v6() ? AF_INET6 : AF_INET;
  fd_ = ::socket(family, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd_ < 0) {
    return std::error_code(errno, std::generic_category());
  }

  // Set non-blocking
  if (auto ec = set_nonblocking(); ec) {
    ::close(fd_);
    fd_ = -1;
    return ec;
  }

  // Connect
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

  return create_and_connect(ep);
}

auto tcp_socket_impl::read_some(std::span<char> buffer) -> std::pair<std::error_code, std::size_t> {
  if (!is_open()) {
    return {make_error_code(error::not_connected), 0};
  }

  ssize_t n = ::recv(fd_, buffer.data(), buffer.size(), 0);

  if (n < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return {make_error_code(error::operation_aborted), 0};
    }
    return {std::error_code(errno, std::generic_category()), 0};
  }

  if (n == 0) {
    return {make_error_code(error::eof), 0};
  }

  return {{}, static_cast<std::size_t>(n)};
}

auto tcp_socket_impl::write_some(std::span<char const> buffer)
    -> std::pair<std::error_code, std::size_t> {
  if (!is_open()) {
    return {make_error_code(error::not_connected), 0};
  }

  ssize_t n = ::send(fd_, buffer.data(), buffer.size(), MSG_NOSIGNAL);

  if (n < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return {make_error_code(error::operation_aborted), 0};
    }
    return {std::error_code(errno, std::generic_category()), 0};
  }

  return {{}, static_cast<std::size_t>(n)};
}

void tcp_socket_impl::set_option_nodelay(bool enable) {
  if (!is_open()) {
    throw std::system_error(make_error_code(error::not_connected));
  }

  int flag = enable ? 1 : 0;
  if (::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag)) < 0) {
    throw std::system_error(errno, std::generic_category());
  }
}

void tcp_socket_impl::set_option_keepalive(bool enable) {
  if (!is_open()) {
    throw std::system_error(make_error_code(error::not_connected));
  }

  int flag = enable ? 1 : 0;
  if (::setsockopt(fd_, SOL_SOCKET, SO_KEEPALIVE, &flag, sizeof(flag)) < 0) {
    throw std::system_error(errno, std::generic_category());
  }
}

void tcp_socket_impl::set_option_reuseaddr(bool enable) {
  if (!is_open()) {
    throw std::system_error(make_error_code(error::not_connected));
  }

  int flag = enable ? 1 : 0;
  if (::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag)) < 0) {
    throw std::system_error(errno, std::generic_category());
  }
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

auto tcp_socket_impl::local_endpoint() const -> ip::tcp_endpoint {
  if (!is_open()) {
    throw std::system_error(make_error_code(error::not_connected));
  }

  sockaddr_storage addr{};
  socklen_t len = sizeof(addr);

  if (::getsockname(fd_, reinterpret_cast<sockaddr*>(&addr), &len) < 0) {
    throw std::system_error(errno, std::generic_category());
  }

  return sockaddr_to_endpoint(addr);
}

auto tcp_socket_impl::remote_endpoint() const -> ip::tcp_endpoint {
  if (!is_open()) {
    throw std::system_error(make_error_code(error::not_connected));
  }

  sockaddr_storage addr{};
  socklen_t len = sizeof(addr);

  if (::getpeername(fd_, reinterpret_cast<sockaddr*>(&addr), &len) < 0) {
    throw std::system_error(errno, std::generic_category());
  }

  return sockaddr_to_endpoint(addr);
}

}  // namespace xz::io::detail

// tcp_socket implementation

namespace xz::io {

tcp_socket::tcp_socket(io_context& ctx) : impl_(std::make_unique<detail::tcp_socket_impl>(ctx)) {}

tcp_socket::~tcp_socket() = default;

tcp_socket::tcp_socket(tcp_socket&&) noexcept = default;

auto tcp_socket::operator=(tcp_socket&&) noexcept -> tcp_socket& = default;

auto tcp_socket::get_executor() noexcept -> io_context& { return impl_->get_executor(); }

auto tcp_socket::is_open() const noexcept -> bool { return impl_->is_open(); }

auto tcp_socket::native_handle() const noexcept -> int { return impl_->native_handle(); }

void tcp_socket::close() { impl_->close(); }

void tcp_socket::close(std::error_code& ec) noexcept { impl_->close(ec); }

void tcp_socket::set_option_nodelay(bool enable) { impl_->set_option_nodelay(enable); }

void tcp_socket::set_option_keepalive(bool enable) { impl_->set_option_keepalive(enable); }

void tcp_socket::set_option_reuseaddr(bool enable) { impl_->set_option_reuseaddr(enable); }

auto tcp_socket::local_endpoint() const -> ip::tcp_endpoint { return impl_->local_endpoint(); }

auto tcp_socket::remote_endpoint() const -> ip::tcp_endpoint { return impl_->remote_endpoint(); }

// Async operation implementations

tcp_socket::async_connect_op::async_connect_op(tcp_socket& s, ip::tcp_endpoint ep,
                                                std::chrono::milliseconds timeout,
                                                std::stop_token stop)
    : awaitable_op<void>(std::move(stop)), socket_(s), endpoint_(ep), timeout_(timeout) {}

void tcp_socket::async_connect_op::cleanup_timer() {
  if (timer_id_ != 0) {
    socket_.get_executor().cancel_timer(timer_id_);
    timer_id_ = 0;
  }
}

void tcp_socket::async_connect_op::start_operation() {
  // Check if already cancelled
  if (stop_requested()) {
    complete(make_error_code(error::operation_aborted));
    return;
  }

  auto ec = socket_.impl_->connect(endpoint_);
  if (ec && ec != std::errc::operation_in_progress) {
    complete(ec);
    return;
  }

  // Setup timeout if specified
  if (timeout_.count() > 0) {
    timer_id_ = socket_.get_executor().schedule_timer(timeout_, [this]() {
      // Timeout fired - cancel the connect
      socket_.get_executor().deregister_fd(socket_.native_handle());
      timer_id_ = 0;
      complete(make_error_code(error::timeout));
    });
  }

  // Register for write events (connection completion)
  struct connect_operation : io_context::operation_base {
    tcp_socket& socket;
    tcp_socket::async_connect_op& op;

    connect_operation(tcp_socket& s, tcp_socket::async_connect_op& o) : socket(s), op(o) {}

    void execute() override {
      // Check connection status
      int error = 0;
      socklen_t len = sizeof(error);
      ::getsockopt(socket.native_handle(), SOL_SOCKET, SO_ERROR, &error, &len);

      socket.get_executor().deregister_fd(socket.native_handle());

      // Cancel timeout timer
      op.cleanup_timer();

      if (error) {
        op.complete(std::error_code(error, std::generic_category()));
      } else {
        op.complete({});
      }
    }
  };

  socket_.get_executor().register_fd_write(
      socket_.native_handle(),
      std::make_unique<connect_operation>(socket_, *this));
}

tcp_socket::async_read_some_op::async_read_some_op(tcp_socket& s, std::span<char> buf,
                                                    std::chrono::milliseconds timeout,
                                                    std::stop_token stop)
    : awaitable_op<std::size_t>(std::move(stop)), socket_(s), buffer_(buf), timeout_(timeout) {}

void tcp_socket::async_read_some_op::cleanup_timer() {
  if (timer_id_ != 0) {
    socket_.get_executor().cancel_timer(timer_id_);
    timer_id_ = 0;
  }
}

void tcp_socket::async_read_some_op::start_operation() {
  // Check if already cancelled
  if (stop_requested()) {
    complete(make_error_code(error::operation_aborted), 0);
    return;
  }

  // Try immediate read
  auto [ec, n] = socket_.impl_->read_some(buffer_);
  if (!ec || ec != make_error_code(error::operation_aborted)) {
    complete(ec, n);
    return;
  }

  // Setup timeout if specified
  if (timeout_.count() > 0) {
    timer_id_ = socket_.get_executor().schedule_timer(timeout_, [this]() {
      socket_.get_executor().deregister_fd(socket_.native_handle());
      timer_id_ = 0;
      complete(make_error_code(error::timeout), 0);
    });
  }

  // Register for read events
  struct read_operation : io_context::operation_base {
    tcp_socket& socket;
    std::span<char> buffer;
    tcp_socket::async_read_some_op& op;

    read_operation(tcp_socket& s, std::span<char> buf, tcp_socket::async_read_some_op& o)
        : socket(s), buffer(buf), op(o) {}

    void execute() override {
      auto [ec, n] = socket.impl_->read_some(buffer);
      socket.get_executor().deregister_fd(socket.native_handle());

      // Cancel timeout timer
      op.cleanup_timer();

      op.complete(ec, n);
    }
  };

  socket_.get_executor().register_fd_read(
      socket_.native_handle(),
      std::make_unique<read_operation>(socket_, buffer_, *this));
}

tcp_socket::async_write_some_op::async_write_some_op(tcp_socket& s, std::span<char const> buf,
                                                      std::chrono::milliseconds timeout,
                                                      std::stop_token stop)
    : awaitable_op<std::size_t>(std::move(stop)), socket_(s), buffer_(buf), timeout_(timeout) {}

void tcp_socket::async_write_some_op::cleanup_timer() {
  if (timer_id_ != 0) {
    socket_.get_executor().cancel_timer(timer_id_);
    timer_id_ = 0;
  }
}

void tcp_socket::async_write_some_op::start_operation() {
  // Check if already cancelled
  if (stop_requested()) {
    complete(make_error_code(error::operation_aborted), 0);
    return;
  }

  // Try immediate write
  auto [ec, n] = socket_.impl_->write_some(buffer_);
  if (!ec || ec != make_error_code(error::operation_aborted)) {
    complete(ec, n);
    return;
  }

  // Setup timeout if specified
  if (timeout_.count() > 0) {
    timer_id_ = socket_.get_executor().schedule_timer(timeout_, [this]() {
      socket_.get_executor().deregister_fd(socket_.native_handle());
      timer_id_ = 0;
      complete(make_error_code(error::timeout), 0);
    });
  }

  // Register for write events
  struct write_operation : io_context::operation_base {
    tcp_socket& socket;
    std::span<char const> buffer;
    tcp_socket::async_write_some_op& op;

    write_operation(tcp_socket& s, std::span<char const> buf, tcp_socket::async_write_some_op& o)
        : socket(s), buffer(buf), op(o) {}

    void execute() override {
      auto [ec, n] = socket.impl_->write_some(buffer);
      socket.get_executor().deregister_fd(socket.native_handle());

      // Cancel timeout timer
      op.cleanup_timer();

      op.complete(ec, n);
    }
  };

  socket_.get_executor().register_fd_write(
      socket_.native_handle(),
      std::make_unique<write_operation>(socket_, buffer_, *this));
}

}  // namespace xz::io

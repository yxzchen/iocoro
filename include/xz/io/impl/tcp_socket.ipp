
#include <xz/io/tcp_socket.hpp>
#include <xz/io/detail/tcp_socket_impl.hpp>
#include <xz/io/error.hpp>
#include <xz/io/detail/current_executor.hpp>
#include <xz/io/detail/operation_base.hpp>
#include <xz/io/when_any.hpp>

#include <cerrno>
#include <cstring>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace xz::io {

}  // namespace xz::io

namespace xz::io::detail {

struct fd_wait_state {
  io_context* ctx = nullptr;
  int fd = -1;
  bool write = false;

  std::coroutine_handle<> h{};
  bool done = false;
  std::error_code ec{};

  void complete(std::error_code e) {
    if (done) return;
    done = true;
    ec = e;
    if (h) {
      defer_resume(h);
    }
  }
};

struct fd_wait_operation : operation_base {
  std::shared_ptr<fd_wait_state> st;

  explicit fd_wait_operation(std::shared_ptr<fd_wait_state> s) : st(std::move(s)) {}

  void execute() override { st->complete({}); }

  void abort(std::error_code ec) override { st->complete(ec); }
};

struct fd_wait_awaiter {
  std::shared_ptr<fd_wait_state> st;

  auto await_ready() const noexcept -> bool { return st->done; }

  auto await_suspend(std::coroutine_handle<> h) -> bool {
    st->h = h;
    if (st->write) {
      st->ctx->register_fd_write(st->fd, std::make_unique<fd_wait_operation>(st));
    } else {
      st->ctx->register_fd_read(st->fd, std::make_unique<fd_wait_operation>(st));
    }
    return true;
  }

  void await_resume() noexcept {}
};

inline auto wait_readable(io_context& ctx, int fd) -> awaitable<void> {
  auto st = std::make_shared<fd_wait_state>();
  st->ctx = &ctx;
  st->fd = fd;
  st->write = false;
  co_await fd_wait_awaiter{std::move(st)};
}

inline auto wait_writable(io_context& ctx, int fd) -> awaitable<void> {
  auto st = std::make_shared<fd_wait_state>();
  st->ctx = &ctx;
  st->fd = fd;
  st->write = true;
  co_await fd_wait_awaiter{std::move(st)};
}

struct timer_wait_state {
  io_context* ctx = nullptr;
  detail::timer_handle handle{};
  std::coroutine_handle<> h{};
  bool done = false;

  void complete() {
    if (done) return;
    done = true;
    if (h) {
      defer_resume(h);
    }
  }

  void cancel() {
    if (done) return;
    done = true;
    if (handle && ctx) {
      ctx->cancel_timer(handle);
    }
    if (h) {
      defer_resume(h);
    }
  }
};

struct timer_awaiter {
  std::shared_ptr<timer_wait_state> st;
  std::chrono::milliseconds duration;

  auto await_ready() const noexcept -> bool { return duration.count() <= 0; }

  auto await_suspend(std::coroutine_handle<> h) -> bool {
    st->h = h;
    st->handle = st->ctx->schedule_timer(duration, [s = st]() { s->complete(); });
    return true;
  }

  void await_resume() noexcept {}
};

struct timeout_op {
  std::shared_ptr<timer_wait_state> st;
  std::chrono::milliseconds duration{};

  auto task() -> awaitable<void> {
    if (duration.count() <= 0) co_return;
    co_await timer_awaiter{st, duration};
  }

  void cancel() {
    if (st) st->cancel();
  }
};

[[noreturn]] inline void throw_timeout() {
  throw std::system_error(error::timeout);
}

void tcp_socket_impl::close() {
  if (fd_ >= 0) {
    ctx_.deregister_fd(fd_);
    ::close(fd_);
    fd_ = -1;
  }
}

auto tcp_socket_impl::close_nothrow() noexcept -> std::error_code {
  try {
    close();
    return {};
  } catch (std::system_error const& e) {
    return e.code();
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

auto tcp_socket_impl::connect(ip::tcp_endpoint const& ep) noexcept -> std::error_code {
  if (is_open()) {
    return error::already_connected;
  }

  auto ec = create_and_connect(ep);
  if (ec) {
    return ec;
  }
  return {};
}

auto tcp_socket_impl::read_some(std::span<char> buffer) noexcept -> expected<std::size_t, std::error_code> {
  if (!is_open()) {
    return unexpected(error::not_connected);
  }

  ssize_t n = ::recv(fd_, buffer.data(), buffer.size(), 0);

  if (n < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return unexpected(std::make_error_code(std::errc::operation_would_block));
    }
    return unexpected(std::error_code(errno, std::generic_category()));
  }

  if (n == 0) {
    return unexpected(error::eof);
  }

  return static_cast<std::size_t>(n);
}

auto tcp_socket_impl::write_some(std::span<char const> buffer) noexcept -> expected<std::size_t, std::error_code> {
  if (!is_open()) {
    return unexpected(error::not_connected);
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

auto tcp_socket_impl::set_option_nodelay(bool enable) -> std::error_code {
  if (!is_open()) {
    return error::not_connected;
  }

  int flag = enable ? 1 : 0;
  if (::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag)) < 0) {
    return std::error_code(errno, std::generic_category());
  }
  return {};
}

auto tcp_socket_impl::set_option_keepalive(bool enable) -> std::error_code {
  if (!is_open()) {
    return error::not_connected;
  }

  int flag = enable ? 1 : 0;
  if (::setsockopt(fd_, SOL_SOCKET, SO_KEEPALIVE, &flag, sizeof(flag)) < 0) {
    return std::error_code(errno, std::generic_category());
  }
  return {};
}

auto tcp_socket_impl::set_option_reuseaddr(bool enable) -> std::error_code {
  if (!is_open()) {
    return error::not_connected;
  }

  int flag = enable ? 1 : 0;
  if (::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag)) < 0) {
    return std::error_code(errno, std::generic_category());
  }
  return {};
}

namespace {

auto sockaddr_to_endpoint(sockaddr_storage const& addr) -> expected<ip::tcp_endpoint, std::error_code> {
  if (addr.ss_family == AF_INET) {
    auto* in = reinterpret_cast<sockaddr_in const*>(&addr);
    return ip::tcp_endpoint{ip::address_v4{ntohl(in->sin_addr.s_addr)}, ntohs(in->sin_port)};
  } else if (addr.ss_family == AF_INET6) {
    auto* in6 = reinterpret_cast<sockaddr_in6 const*>(&addr);
    ip::address_v6::bytes_type bytes;
    std::memcpy(bytes.data(), &in6->sin6_addr, 16);
    return ip::tcp_endpoint{ip::address_v6{bytes}, ntohs(in6->sin6_port)};
  }
  return unexpected(error::invalid_argument);
}

}  // namespace

auto tcp_socket_impl::local_endpoint() const -> expected<ip::tcp_endpoint, std::error_code> {
  if (!is_open()) {
    return unexpected(error::not_connected);
  }

  sockaddr_storage addr{};
  socklen_t len = sizeof(addr);

  if (::getsockname(fd_, reinterpret_cast<sockaddr*>(&addr), &len) < 0) {
    return unexpected(std::error_code(errno, std::generic_category()));
  }

  return sockaddr_to_endpoint(addr);
}

auto tcp_socket_impl::remote_endpoint() const -> expected<ip::tcp_endpoint, std::error_code> {
  if (!is_open()) {
    return unexpected(error::not_connected);
  }

  sockaddr_storage addr{};
  socklen_t len = sizeof(addr);

  if (::getpeername(fd_, reinterpret_cast<sockaddr*>(&addr), &len) < 0) {
    return unexpected(std::error_code(errno, std::generic_category()));
  }

  return sockaddr_to_endpoint(addr);
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

auto tcp_socket::close_nothrow() noexcept -> std::error_code { return socket_impl_->close_nothrow(); }

auto tcp_socket::set_option_nodelay(bool enable) -> std::error_code {
  return socket_impl_->set_option_nodelay(enable);
}

auto tcp_socket::set_option_keepalive(bool enable) -> std::error_code {
  return socket_impl_->set_option_keepalive(enable);
}

auto tcp_socket::set_option_reuseaddr(bool enable) -> std::error_code {
  return socket_impl_->set_option_reuseaddr(enable);
}

auto tcp_socket::local_endpoint() const -> expected<ip::tcp_endpoint, std::error_code> {
  return socket_impl_->local_endpoint();
}

auto tcp_socket::remote_endpoint() const -> expected<ip::tcp_endpoint, std::error_code> {
  return socket_impl_->remote_endpoint();
}

auto tcp_socket::async_connect(ip::tcp_endpoint ep, std::chrono::milliseconds timeout) -> awaitable<void> {
  auto impl = socket_impl_;
  auto& ctx = impl->get_executor();

  auto ec = impl->connect(ep);
  if (ec) {
    throw std::system_error(ec);
  }

  int fd = impl->native_handle();
  if (fd < 0) {
    throw std::system_error(error::not_connected);
  }

  if (timeout.count() > 0) {
    detail::timeout_op to{std::make_shared<detail::timer_wait_state>(), timeout};
    to.st->ctx = &ctx;
    auto [idx, result] = co_await when_any(detail::wait_writable(ctx, fd), to.task());
    if (idx == 1) {
      ctx.deregister_fd(fd);
      detail::throw_timeout();
    }
    // Cancel the losing timer so it doesn't keep io_context alive until expiry.
    to.cancel();
    (void)result;
  } else {
    co_await detail::wait_writable(ctx, fd);
  }

  int so_error = 0;
  socklen_t len = sizeof(so_error);
  ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &len);
  if (so_error) {
    throw std::system_error(std::error_code(so_error, std::generic_category()));
  }
}

auto tcp_socket::async_read_some(std::span<char> buffer, std::chrono::milliseconds timeout) -> awaitable<std::size_t> {
  auto impl = socket_impl_;
  auto& ctx = impl->get_executor();

  for (;;) {
    auto result = impl->read_some(buffer);
    if (result) {
      co_return *result;
    }

    auto ec = result.error();
    if (ec == std::make_error_code(std::errc::operation_would_block)) {
      int fd = impl->native_handle();
      if (fd < 0) {
        throw std::system_error(error::not_connected);
      }

      if (timeout.count() > 0) {
        detail::timeout_op to{std::make_shared<detail::timer_wait_state>(), timeout};
        to.st->ctx = &ctx;
        auto [idx, v] = co_await when_any(detail::wait_readable(ctx, fd), to.task());
        if (idx == 1) {
          ctx.deregister_fd(fd);
          detail::throw_timeout();
        }
        to.cancel();
        (void)v;
      } else {
        co_await detail::wait_readable(ctx, fd);
      }
      continue;
    }

    throw std::system_error(ec);
  }
}

auto tcp_socket::async_write_some(std::span<char const> buffer, std::chrono::milliseconds timeout)
    -> awaitable<std::size_t> {
  auto impl = socket_impl_;
  auto& ctx = impl->get_executor();

  for (;;) {
    auto result = impl->write_some(buffer);
    if (result) {
      co_return *result;
    }

    auto ec = result.error();
    if (ec == std::make_error_code(std::errc::operation_would_block)) {
      int fd = impl->native_handle();
      if (fd < 0) {
        throw std::system_error(error::not_connected);
      }

      if (timeout.count() > 0) {
        detail::timeout_op to{std::make_shared<detail::timer_wait_state>(), timeout};
        to.st->ctx = &ctx;
        auto [idx, v] = co_await when_any(detail::wait_writable(ctx, fd), to.task());
        if (idx == 1) {
          ctx.deregister_fd(fd);
          detail::throw_timeout();
        }
        to.cancel();
        (void)v;
      } else {
        co_await detail::wait_writable(ctx, fd);
      }
      continue;
    }

    throw std::system_error(ec);
  }
}

}  // namespace xz::io

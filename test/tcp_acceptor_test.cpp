#include <gtest/gtest.h>

#include <iocoro/co_sleep.hpp>
#include <iocoro/co_spawn.hpp>
#include <iocoro/impl.hpp>
#include <iocoro/io_context.hpp>
#include <iocoro/ip/address.hpp>
#include <iocoro/ip/tcp/acceptor.hpp>
#include <iocoro/ip/tcp/endpoint.hpp>
#include <iocoro/ip/tcp/socket.hpp>
#include <iocoro/socket_option.hpp>

#include "test_util.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <future>
#include <optional>
#include <string>
#include <thread>
#include <utility>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>

namespace {

using namespace std::chrono_literals;

struct unique_fd {
  int fd{-1};
  unique_fd() = default;
  explicit unique_fd(int f) : fd(f) {}
  unique_fd(unique_fd const&) = delete;
  auto operator=(unique_fd const&) -> unique_fd& = delete;
  unique_fd(unique_fd&& o) noexcept : fd(std::exchange(o.fd, -1)) {}
  auto operator=(unique_fd&& o) noexcept -> unique_fd& {
    if (this != &o) {
      reset();
      fd = std::exchange(o.fd, -1);
    }
    return *this;
  }
  ~unique_fd() { reset(); }
  void reset(int f = -1) noexcept {
    if (fd >= 0) ::close(fd);
    fd = f;
  }
  explicit operator bool() const noexcept { return fd >= 0; }
};

static auto connect_to(std::uint16_t port) -> unique_fd {
  int fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (fd < 0) return unique_fd{};

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

  if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    ::close(fd);
    return unique_fd{};
  }
  return unique_fd{fd};
}

static auto local_port(int fd) -> std::optional<std::uint16_t> {
  if (fd < 0) return std::nullopt;
  sockaddr_in addr{};
  socklen_t len = sizeof(addr);
  if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) return std::nullopt;
  return static_cast<std::uint16_t>(ntohs(addr.sin_port));
}

static auto read_exact(int fd, void* data, std::size_t n) -> bool {
  auto* p = static_cast<std::byte*>(data);
  std::size_t off = 0;
  while (off < n) {
    auto r = ::read(fd, p + off, n - off);
    if (r > 0) {
      off += static_cast<std::size_t>(r);
      continue;
    }
    if (r == 0) return false;
    if (errno == EINTR) continue;
    return false;
  }
  return true;
}

static auto write_all(int fd, void const* data, std::size_t n) -> bool {
  auto const* p = static_cast<std::byte const*>(data);
  std::size_t off = 0;
  while (off < n) {
    auto r = ::write(fd, p + off, n - off);
    if (r >= 0) {
      off += static_cast<std::size_t>(r);
      continue;
    }
    if (errno == EINTR) continue;
    return false;
  }
  return true;
}

TEST(tcp_acceptor_test, construction_and_executor) {
  iocoro::io_context ctx;
  auto ex = ctx.get_executor();

  iocoro::ip::tcp::acceptor a{ctx};
  EXPECT_EQ(a.get_executor(), ex);
  EXPECT_FALSE(a.is_open());
  EXPECT_LT(a.native_handle(), 0);
}

TEST(tcp_acceptor_test, async_accept_without_open_returns_not_open) {
  iocoro::io_context ctx;
  iocoro::ip::tcp::acceptor a{ctx};

  auto r = iocoro::sync_wait_for(ctx, 200ms, a.async_accept());
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error(), iocoro::error::not_open);
}

TEST(tcp_acceptor_test, open_without_listen_async_accept_returns_not_listening) {
  iocoro::io_context ctx;
  iocoro::ip::tcp::acceptor a{ctx};

  auto ec = a.open(AF_INET);
  ASSERT_FALSE(ec) << ec.message();

  auto r = iocoro::sync_wait_for(ctx, 200ms, a.async_accept());
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error(), iocoro::error::not_listening);
}

TEST(tcp_acceptor_test, bind_listen_local_endpoint_errors_when_not_open) {
  iocoro::io_context ctx;
  iocoro::ip::tcp::acceptor a{ctx};

  iocoro::ip::tcp::endpoint ep{iocoro::ip::address_v4::loopback(), 0};

  auto ec = a.bind(ep);
  EXPECT_EQ(ec, iocoro::error::not_open);

  ec = a.listen(16);
  EXPECT_EQ(ec, iocoro::error::not_open);

  auto le = a.local_endpoint();
  EXPECT_FALSE(le);
  EXPECT_EQ(le.error(), iocoro::error::not_open);
}

TEST(tcp_acceptor_test, open_invalid_family_returns_error) {
  iocoro::io_context ctx;
  iocoro::ip::tcp::acceptor a{ctx};

  auto ec = a.open(-1);
  EXPECT_TRUE(static_cast<bool>(ec));
}

TEST(tcp_acceptor_test, open_bind_listen_accept_and_exchange_data) {
  iocoro::io_context ctx;
  auto ex = ctx.get_executor();

  auto ec = iocoro::sync_wait_for(ctx, 1s, [&]() -> iocoro::awaitable<std::error_code> {
    iocoro::ip::tcp::acceptor a{ex};

    if (auto e = a.open(AF_INET)) co_return e;
    (void)a.set_option(iocoro::socket_option::reuse_address{true});

    if (auto e = a.bind(iocoro::ip::tcp::endpoint{iocoro::ip::address_v4::loopback(), 0})) {
      co_return e;
    }
    if (auto e = a.listen(16)) co_return e;

    auto le = a.local_endpoint();
    if (!le) co_return le.error();
    if (le->port() == 0) co_return iocoro::make_error_code(iocoro::error::invalid_argument);

    unique_fd c = connect_to(le->port());
    if (!c) co_return std::error_code(errno, std::generic_category());

    auto accepted = co_await a.async_accept();
    if (!accepted) co_return accepted.error();

    auto s = std::move(*accepted);
    if (!s.is_open() || !s.is_connected()) {
      co_return iocoro::make_error_code(iocoro::error::invalid_argument);
    }

    auto re = s.remote_endpoint();
    if (!re) co_return re.error();
    if (re->family() != AF_INET) co_return iocoro::make_error_code(iocoro::error::invalid_argument);
    if (re->address().to_string() != "127.0.0.1") {
      co_return iocoro::make_error_code(iocoro::error::invalid_argument);
    }

    auto cport = local_port(c.fd);
    if (!cport.has_value()) co_return iocoro::make_error_code(iocoro::error::invalid_argument);
    if (re->port() != *cport) co_return iocoro::make_error_code(iocoro::error::invalid_argument);

    // Client -> server
    {
      char const msg[] = "hi";
      if (!write_all(c.fd, msg, sizeof(msg) - 1)) {
        co_return std::error_code(errno, std::generic_category());
      }
      std::array<std::byte, 2> buf{};
      auto rr = co_await s.async_read_some(buf);
      if (!rr) co_return rr.error();
      if (*rr != 2U) co_return iocoro::make_error_code(iocoro::error::invalid_argument);
      if (static_cast<char>(buf[0]) != 'h' || static_cast<char>(buf[1]) != 'i') {
        co_return iocoro::make_error_code(iocoro::error::invalid_argument);
      }
    }

    // Server -> client
    {
      std::array<std::byte, 2> out{std::byte{'o'}, std::byte{'k'}};
      auto wr = co_await s.async_write_some(out);
      if (!wr) co_return wr.error();
      if (*wr != 2U) co_return iocoro::make_error_code(iocoro::error::invalid_argument);

      char got[2]{};
      if (!read_exact(c.fd, got, 2)) co_return std::error_code(errno, std::generic_category());
      if (got[0] != 'o' || got[1] != 'k') co_return iocoro::make_error_code(iocoro::error::invalid_argument);
    }

    co_return std::error_code{};
  }());
  ASSERT_FALSE(ec) << ec.message();
}

TEST(tcp_acceptor_test, cancel_aborts_waiting_accept) {
  iocoro::io_context ctx;
  auto ex = ctx.get_executor();

  auto got = iocoro::sync_wait_for(ctx, 1s, [&]() -> iocoro::awaitable<std::error_code> {
    iocoro::ip::tcp::acceptor a{ex};
    if (auto ec = a.open(AF_INET)) co_return ec;
    if (auto ec = a.bind(iocoro::ip::tcp::endpoint{iocoro::ip::address_v4::loopback(), 0})) co_return ec;
    if (auto ec = a.listen(16)) co_return ec;

    std::error_code out{};
    auto task = iocoro::co_spawn(
      ex,
      [&]() -> iocoro::awaitable<void> {
        auto r = co_await a.async_accept();
        if (!r) out = r.error();
      },
      iocoro::use_awaitable);

    // Give async_accept a chance to run accept()->EAGAIN and arm wait_read_ready().
    (void)co_await iocoro::co_sleep(10ms);
    a.cancel();

    try {
      co_await std::move(task);
    } catch (...) {
    }

    co_return out;
  }());

  EXPECT_EQ(got, iocoro::error::operation_aborted);
}

TEST(tcp_acceptor_test, close_aborts_waiting_accept) {
  iocoro::io_context ctx;
  auto ex = ctx.get_executor();

  auto got = iocoro::sync_wait_for(ctx, 1s, [&]() -> iocoro::awaitable<std::error_code> {
    iocoro::ip::tcp::acceptor a{ex};
    if (auto ec = a.open(AF_INET)) co_return ec;
    if (auto ec = a.bind(iocoro::ip::tcp::endpoint{iocoro::ip::address_v4::loopback(), 0})) co_return ec;
    if (auto ec = a.listen(16)) co_return ec;

    std::error_code out{};
    auto task = iocoro::co_spawn(
      ex,
      [&]() -> iocoro::awaitable<void> {
        auto r = co_await a.async_accept();
        if (!r) out = r.error();
      },
      iocoro::use_awaitable);

    (void)co_await iocoro::co_sleep(10ms);
    a.close();

    try {
      co_await std::move(task);
    } catch (...) {
    }

    co_return out;
  }());

  EXPECT_EQ(got, iocoro::error::operation_aborted);
}

TEST(tcp_acceptor_test, multiple_async_accept_are_queued_and_all_succeed) {
  iocoro::io_context ctx;
  auto ex = ctx.get_executor();

  auto ec = iocoro::sync_wait_for(ctx, 2s, [&]() -> iocoro::awaitable<std::error_code> {
    iocoro::ip::tcp::acceptor a{ex};
    if (auto e = a.open(AF_INET)) co_return e;
    if (auto e = a.bind(iocoro::ip::tcp::endpoint{iocoro::ip::address_v4::loopback(), 0})) co_return e;
    if (auto e = a.listen(16)) co_return e;

    auto le = a.local_endpoint();
    if (!le) co_return le.error();
    auto port = le->port();

    std::optional<std::uint16_t> r1{};
    std::optional<std::uint16_t> r2{};
    std::error_code e1{};
    std::error_code e2{};

    auto t1 = iocoro::co_spawn(
      ex,
      [&]() -> iocoro::awaitable<void> {
        auto r = co_await a.async_accept();
        if (!r) {
          e1 = r.error();
          co_return;
        }
        auto re = r->remote_endpoint();
        if (!re) {
          e1 = re.error();
          co_return;
        }
        r1 = re->port();
      },
      iocoro::use_awaitable);

    auto t2 = iocoro::co_spawn(
      ex,
      [&]() -> iocoro::awaitable<void> {
        auto r = co_await a.async_accept();
        if (!r) {
          e2 = r.error();
          co_return;
        }
        auto re = r->remote_endpoint();
        if (!re) {
          e2 = re.error();
          co_return;
        }
        r2 = re->port();
      },
      iocoro::use_awaitable);

    // Connect two clients (sequentially).
    unique_fd c1 = connect_to(port);
    if (!c1) co_return std::error_code(errno, std::generic_category());
    unique_fd c2 = connect_to(port);
    if (!c2) co_return std::error_code(errno, std::generic_category());

    try {
      co_await std::move(t1);
      co_await std::move(t2);
    } catch (...) {
    }

    if (e1) co_return e1;
    if (e2) co_return e2;
    if (!r1.has_value() || !r2.has_value()) co_return iocoro::make_error_code(iocoro::error::invalid_argument);
    if (*r1 == 0 || *r2 == 0) co_return iocoro::make_error_code(iocoro::error::invalid_argument);
    if (*r1 == *r2) co_return iocoro::make_error_code(iocoro::error::invalid_argument);

    co_return std::error_code{};
  }());
  ASSERT_FALSE(ec) << ec.message();
}

}  // namespace



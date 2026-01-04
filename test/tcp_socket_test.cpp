#include <gtest/gtest.h>

#include <iocoro/co_sleep.hpp>
#include <iocoro/co_spawn.hpp>
#include <iocoro/io/read_until.hpp>
#include <iocoro/io/write.hpp>
#include <iocoro/io_context.hpp>
#include <iocoro/ip.hpp>
#include <iocoro/socket_option.hpp>

#include "test_util.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include <arpa/inet.h>
#include <fcntl.h>
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

static auto make_listen_socket_ipv4(std::uint16_t& port_out) -> unique_fd {
  int fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (fd < 0) return unique_fd{};

  // Best-effort: make accept loop non-blocking and avoid test hangs on failures.
  if (int flags = ::fcntl(fd, F_GETFL, 0); flags >= 0) {
    (void)::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  }

  int one = 1;
  (void)::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(0);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

  if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    ::close(fd);
    return unique_fd{};
  }

  socklen_t len = sizeof(addr);
  if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
    ::close(fd);
    return unique_fd{};
  }

  if (::listen(fd, 16) != 0) {
    ::close(fd);
    return unique_fd{};
  }

  port_out = ntohs(addr.sin_port);
  return unique_fd{fd};
}

static auto as_bytes(std::string const& s) -> std::span<std::byte const> {
  return {reinterpret_cast<std::byte const*>(s.data()), s.size()};
}

static auto should_skip_net_error(std::error_code ec) -> bool {
  return ec == std::errc::connection_refused || ec == std::errc::network_unreachable ||
         ec == std::errc::host_unreachable || ec == std::errc::address_not_available;
}

static void fill_send_buffer_nonblocking(int fd) {
  if (fd < 0) return;

  // Try to reduce the send buffer to make EAGAIN easier to trigger in tests.
  // Best-effort: kernels often clamp this value.
  int sndbuf = 1024;
  (void)::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

  std::array<std::byte, 4096> tmp{};
  for (int i = 0; i < 1'000'000; ++i) {
    auto n = ::write(fd, tmp.data(), tmp.size());
    if (n > 0) continue;
    if (n < 0 && errno == EINTR) continue;
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
    return;
  }
}

TEST(tcp_socket_test, construction_and_executor) {
  iocoro::io_context ctx;

  iocoro::ip::tcp::socket s{ctx};
  EXPECT_FALSE(s.is_open());
  EXPECT_FALSE(s.is_connected());
  EXPECT_LT(s.native_handle(), 0);
}

TEST(tcp_socket_test, redis_ping_ipv4_and_endpoints) {
  iocoro::io_context ctx;
  iocoro::ip::tcp::socket s{ctx};

  auto ep_r = iocoro::ip::tcp::endpoint::from_string("127.0.0.1:6379");
  ASSERT_TRUE(ep_r) << ep_r.error().message();
  auto ep = *ep_r;

  auto rr = iocoro::sync_wait_for(
    ctx, 1s, [&]() -> iocoro::awaitable<iocoro::expected<std::string, std::error_code>> {
      auto ec = co_await s.async_connect(ep);
      if (ec) co_return iocoro::unexpected(ec);

      EXPECT_TRUE(s.is_open());
      EXPECT_TRUE(s.is_connected());

      auto le = s.local_endpoint();
      if (!le) co_return iocoro::unexpected(le.error());
      auto re = s.remote_endpoint();
      if (!re) co_return iocoro::unexpected(re.error());

      EXPECT_EQ(re->port(), 6379);
      EXPECT_EQ(re->address().to_string(), "127.0.0.1");
      EXPECT_EQ(re->family(), AF_INET);
      EXPECT_GT(le->port(), 0);

      std::string cmd = "*1\r\n$4\r\nPING\r\n";
      auto wr = co_await iocoro::io::async_write(s, as_bytes(cmd));
      if (!wr) co_return iocoro::unexpected(wr.error());

      std::string out;
      out.reserve(64);
      auto n = co_await iocoro::io::async_read_until(s, out, "\r\n", 4096);
      if (!n) co_return iocoro::unexpected(n.error());
      co_return out.substr(0, *n);
    }());

  if (!rr && should_skip_net_error(rr.error())) {
    GTEST_SKIP() << "redis not reachable on 127.0.0.1:6379: " << rr.error().message();
  }
  ASSERT_TRUE(rr) << rr.error().message();
  ASSERT_GE(rr->size(), 5U);
  EXPECT_TRUE(rr->rfind("+PONG", 0) == 0) << *rr;
}

TEST(tcp_socket_test, set_get_option_tcp_nodelay) {
  iocoro::io_context ctx;
  iocoro::ip::tcp::socket s{ctx};

  auto ep_r = iocoro::ip::tcp::endpoint::from_string("127.0.0.1:6379");
  ASSERT_TRUE(ep_r) << ep_r.error().message();
  auto ep = *ep_r;

  auto connect_ec = iocoro::sync_wait_for(ctx, 1s, s.async_connect(ep));
  if (connect_ec && should_skip_net_error(connect_ec)) {
    GTEST_SKIP() << "redis not reachable on 127.0.0.1:6379: " << connect_ec.message();
  }
  ASSERT_FALSE(connect_ec) << connect_ec.message();

  iocoro::socket_option::tcp::no_delay nd{true};
  auto ec = s.set_option(nd);
  ASSERT_FALSE(ec) << ec.message();

  iocoro::socket_option::tcp::no_delay nd2{};
  ec = s.get_option(nd2);
  ASSERT_FALSE(ec) << ec.message();
  EXPECT_TRUE(nd2.enabled());
}

TEST(tcp_socket_test, close_is_idempotent_and_resets_endpoints) {
  iocoro::io_context ctx;
  iocoro::ip::tcp::socket s{ctx};

  auto ep_r = iocoro::ip::tcp::endpoint::from_string("127.0.0.1:6379");
  ASSERT_TRUE(ep_r) << ep_r.error().message();
  auto ep = *ep_r;

  auto connect_ec = iocoro::sync_wait_for(ctx, 1s, s.async_connect(ep));
  if (connect_ec && should_skip_net_error(connect_ec)) {
    GTEST_SKIP() << "redis not reachable on 127.0.0.1:6379: " << connect_ec.message();
  }
  ASSERT_FALSE(connect_ec) << connect_ec.message();

  EXPECT_TRUE(s.is_open());
  EXPECT_TRUE(s.is_connected());

  s.close();
  EXPECT_FALSE(s.is_open());
  EXPECT_FALSE(s.is_connected());
  EXPECT_LT(s.native_handle(), 0);

  auto re = s.remote_endpoint();
  ASSERT_FALSE(re);
  EXPECT_EQ(re.error(), iocoro::error::not_open);

  // idempotent
  s.close();
  EXPECT_FALSE(s.is_open());
}

TEST(tcp_socket_test, cancel_read_aborts_pending_read) {
  iocoro::io_context ctx;
  auto ex = ctx.get_executor();

  std::uint16_t port = 0;
  auto listen_fd = make_listen_socket_ipv4(port);
  if (!listen_fd) {
    GTEST_SKIP() << "failed to create/bind/listen on 127.0.0.1";
  }

  // Accept in the background so connect can complete; keep peer open but don't send data.
  std::promise<unique_fd> accepted_p;
  auto accepted_f = accepted_p.get_future();
  std::thread accept_thread([lfd = listen_fd.fd, p = std::move(accepted_p)]() mutable {
    for (int i = 0; i < 1000; ++i) {
      sockaddr_in peer{};
      socklen_t len = sizeof(peer);
      int cfd = ::accept(lfd, reinterpret_cast<sockaddr*>(&peer), &len);
      if (cfd >= 0) {
        p.set_value(unique_fd{cfd});
        return;
      }
      if (errno == EINTR) continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
        continue;
      }
      break;
    }
    p.set_value(unique_fd{});
  });

  iocoro::ip::tcp::socket s{ex};
  auto ep_r =
    iocoro::ip::tcp::endpoint::from_string(std::string("127.0.0.1:") + std::to_string(port));
  ASSERT_TRUE(ep_r) << ep_r.error().message();
  auto ep = *ep_r;

  auto read_ec = iocoro::sync_wait_for(ctx, 500ms, [&]() -> iocoro::awaitable<std::error_code> {
    auto ec = co_await s.async_connect(ep);
    if (ec) co_return ec;

    std::array<std::byte, 1> b{};
    std::atomic<bool> started{false};
    std::error_code got{};

    auto read_task = iocoro::co_spawn(
      ex,
      [&]() -> iocoro::awaitable<void> {
        started.store(true, std::memory_order_release);
        auto rr = co_await s.async_read_some(b);
        if (!rr) got = rr.error();
      },
      iocoro::use_awaitable);

    for (int i = 0; i < 50 && !started.load(std::memory_order_acquire); ++i) {
      (void)co_await iocoro::co_sleep(1ms);
    }
    (void)co_await iocoro::co_sleep(5ms);

    s.cancel_read();

    try {
      co_await std::move(read_task);
    } catch (...) {
    }

    co_return got;
  }());

  EXPECT_EQ(read_ec, iocoro::error::operation_aborted);

  (void)accepted_f.get();
  accept_thread.join();
}

TEST(tcp_socket_test, cancel_write_aborts_pending_write) {
  iocoro::io_context ctx;
  auto ex = ctx.get_executor();

  std::uint16_t port = 0;
  auto listen_fd = make_listen_socket_ipv4(port);
  if (!listen_fd) {
    GTEST_SKIP() << "failed to create/bind/listen on 127.0.0.1";
  }

  std::promise<unique_fd> accepted_p;
  auto accepted_f = accepted_p.get_future();
  std::thread accept_thread([lfd = listen_fd.fd, p = std::move(accepted_p)]() mutable {
    for (int i = 0; i < 1000; ++i) {
      sockaddr_in peer{};
      socklen_t len = sizeof(peer);
      int cfd = ::accept(lfd, reinterpret_cast<sockaddr*>(&peer), &len);
      if (cfd >= 0) {
        // Make peer non-blocking and never read: helps fill client's send buffer.
        if (int flags = ::fcntl(cfd, F_GETFL, 0); flags >= 0) {
          (void)::fcntl(cfd, F_SETFL, flags | O_NONBLOCK);
        }
        p.set_value(unique_fd{cfd});
        return;
      }
      if (errno == EINTR) continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
        continue;
      }
      break;
    }
    p.set_value(unique_fd{});
  });

  iocoro::ip::tcp::socket s{ex};
  auto ep_r =
    iocoro::ip::tcp::endpoint::from_string(std::string("127.0.0.1:") + std::to_string(port));
  ASSERT_TRUE(ep_r) << ep_r.error().message();
  auto ep = *ep_r;

  auto write_ec = iocoro::sync_wait_for(ctx, 800ms, [&]() -> iocoro::awaitable<std::error_code> {
    auto ec = co_await s.async_connect(ep);
    if (ec) co_return ec;

    fill_send_buffer_nonblocking(s.native_handle());

    std::string payload(64 * 1024, 'x');
    std::error_code got{};

    auto write_task = iocoro::co_spawn(
      ex,
      [&]() -> iocoro::awaitable<void> {
        auto wr = co_await s.async_write_some(as_bytes(payload));
        if (!wr) got = wr.error();
      },
      iocoro::use_awaitable);

    (void)co_await iocoro::co_sleep(10ms);
    s.cancel_write();

    try {
      co_await std::move(write_task);
    } catch (...) {
    }

    co_return got;
  }());

  EXPECT_EQ(write_ec, iocoro::error::operation_aborted);

  (void)accepted_f.get();
  accept_thread.join();
}

}  // namespace

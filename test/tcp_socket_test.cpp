#include <gtest/gtest.h>

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

#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>

namespace {

using namespace std::chrono_literals;

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

      std::array<std::byte, 4096> buf{};
      auto n = co_await iocoro::io::async_read_until(s, std::span{buf}, "\r\n");
      if (!n) co_return iocoro::unexpected(n.error());
      co_return std::string{reinterpret_cast<char const*>(buf.data()), *n};
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

}  // namespace

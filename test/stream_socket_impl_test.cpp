#include <gtest/gtest.h>

#include <iocoro/co_spawn.hpp>
#include <iocoro/detail/socket/stream_socket_impl.hpp>
#include <iocoro/io/async_write.hpp>
#include <iocoro/io_context.hpp>
#include <iocoro/ip/endpoint.hpp>
#include <iocoro/src.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include <netinet/in.h>
#include <sys/socket.h>

namespace {

using namespace std::chrono_literals;

using iocoro::detail::socket::stream_socket_impl;

static auto as_bytes(std::string const& s) -> std::span<std::byte const> {
  return {reinterpret_cast<std::byte const*>(s.data()), s.size()};
}

struct ping_state {
  std::atomic<bool> done{false};
  std::error_code ec{};
  std::string reply{};
};

static auto read_until_crlf(stream_socket_impl& s)
  -> iocoro::awaitable<iocoro::expected<std::string, std::error_code>> {
  std::string out;
  out.reserve(64);

  std::array<std::byte, 256> tmp{};
  while (out.find("\r\n") == std::string::npos) {
    auto r = co_await s.async_read_some(tmp);
    if (!r) {
      co_return iocoro::unexpected<std::error_code>(r.error());
    }
    auto n = *r;
    if (n == 0) {
      break;
    }
    out.append(reinterpret_cast<char const*>(tmp.data()), n);
    if (out.size() > 4096) {
      co_return iocoro::unexpected<std::error_code>(iocoro::error::invalid_argument);
    }
  }
  co_return out;
}

static auto should_skip_net_error(std::error_code ec) -> bool {
  return ec == std::errc::connection_refused || ec == std::errc::network_unreachable ||
         ec == std::errc::host_unreachable || ec == std::errc::address_not_available;
}

TEST(stream_socket_impl_test, redis_ping_ipv4) {
  iocoro::io_context ctx;
  auto ex = ctx.get_executor();

  auto sock = std::make_shared<stream_socket_impl>(ex);
  auto ec = sock->open(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (ec) {
    GTEST_SKIP() << "open(AF_INET, SOCK_STREAM) failed: " << ec.message();
  }

  auto ep_r = iocoro::ip::endpoint::from_string("127.0.0.1:6379");
  ASSERT_TRUE(ep_r.has_value()) << ep_r.error().message();
  auto const ep = *ep_r;
  auto st = std::make_shared<ping_state>();

  iocoro::co_spawn(
    ex,
    [sock, st, ep]() -> iocoro::awaitable<void> {
      st->ec = co_await sock->async_connect(ep.data(), ep.size());
      if (st->ec) {
        st->done.store(true, std::memory_order_relaxed);
        co_return;
      }

      std::string cmd = "*1\r\n$4\r\nPING\r\n";
      auto wr = co_await iocoro::io::async_write(*sock, as_bytes(cmd));
      if (!wr) {
        st->ec = wr.error();
        st->done.store(true, std::memory_order_relaxed);
        co_return;
      }

      auto rr = co_await read_until_crlf(*sock);
      if (!rr) {
        st->ec = rr.error();
        st->done.store(true, std::memory_order_relaxed);
        co_return;
      }
      st->reply = std::move(*rr);
      st->done.store(true, std::memory_order_relaxed);
    }(),
    iocoro::detached);

  (void)ctx.run_for(1s);
  ASSERT_TRUE(st->done.load(std::memory_order_relaxed)) << "timeout waiting for redis ping";

  if (st->ec && should_skip_net_error(st->ec)) {
    GTEST_SKIP() << "redis not reachable on 127.0.0.1:6379: " << st->ec.message();
  }
  ASSERT_FALSE(st->ec) << st->ec.message();
  ASSERT_GE(st->reply.size(), 5U);
  EXPECT_TRUE(st->reply.rfind("+PONG", 0) == 0) << st->reply;
}

TEST(stream_socket_impl_test, redis_ping_ipv6) {
  iocoro::io_context ctx;
  auto ex = ctx.get_executor();

  auto sock = std::make_shared<stream_socket_impl>(ex);
  auto ec = sock->open(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
  if (ec) {
    GTEST_SKIP() << "open(AF_INET6, SOCK_STREAM) failed: " << ec.message();
  }

  auto ep_r = iocoro::ip::endpoint::from_string("[::1]:6379");
  ASSERT_TRUE(ep_r.has_value()) << ep_r.error().message();
  auto const ep = *ep_r;
  auto st = std::make_shared<ping_state>();

  iocoro::co_spawn(
    ex,
    [sock, st, ep]() -> iocoro::awaitable<void> {
      st->ec = co_await sock->async_connect(ep.data(), ep.size());
      if (st->ec) {
        st->done.store(true, std::memory_order_relaxed);
        co_return;
      }

      std::string cmd = "*1\r\n$4\r\nPING\r\n";
      auto wr = co_await iocoro::io::async_write(*sock, as_bytes(cmd));
      if (!wr) {
        st->ec = wr.error();
        st->done.store(true, std::memory_order_relaxed);
        co_return;
      }

      auto rr = co_await read_until_crlf(*sock);
      if (!rr) {
        st->ec = rr.error();
        st->done.store(true, std::memory_order_relaxed);
        co_return;
      }
      st->reply = std::move(*rr);
      st->done.store(true, std::memory_order_relaxed);
    }(),
    iocoro::detached);

  (void)ctx.run_for(1s);
  ASSERT_TRUE(st->done.load(std::memory_order_relaxed)) << "timeout waiting for redis ping";

  if (st->ec && should_skip_net_error(st->ec)) {
    GTEST_SKIP() << "redis not reachable on [::1]:6379: " << st->ec.message();
  }
  ASSERT_FALSE(st->ec) << st->ec.message();
  ASSERT_GE(st->reply.size(), 5U);
  EXPECT_TRUE(st->reply.rfind("+PONG", 0) == 0) << st->reply;
}

}  // namespace



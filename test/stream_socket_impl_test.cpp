#include <gtest/gtest.h>

#include <iocoro/co_sleep.hpp>
#include <iocoro/co_spawn.hpp>
#include <iocoro/detail/socket/stream_socket_impl.hpp>
#include <iocoro/io/read_until.hpp>
#include <iocoro/io/write.hpp>
#include <iocoro/io_context.hpp>
#include <iocoro/ip.hpp>

#include "test_util.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>

namespace {

using namespace std::chrono_literals;

using iocoro::detail::socket::stream_socket_impl;
using iocoro::test::make_listen_socket_ipv4;
using iocoro::test::unique_fd;

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
    // Any other error: stop trying; the test will handle it via later operations.
    return;
  }
}

TEST(stream_socket_impl_test, not_open_errors) {
  iocoro::io_context ctx;
  auto ex = ctx.get_executor();

  stream_socket_impl s{ex};

  std::array<std::byte, 8> tmp{};
  std::string payload = "x";

  auto [connect_ec, read_ec, write_ec] = iocoro::sync_wait(
    ctx, [&]() -> iocoro::awaitable<std::tuple<std::error_code, std::error_code, std::error_code>> {
      sockaddr_in dummy{};
      dummy.sin_family = AF_INET;
      dummy.sin_port = htons(1);
      dummy.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

      auto connect_ec =
        co_await s.async_connect(reinterpret_cast<sockaddr*>(&dummy), sizeof(dummy));

      std::error_code read_ec{};
      std::error_code write_ec{};

      auto rr = co_await s.async_read_some(tmp);
      if (!rr) read_ec = rr.error();

      auto wr = co_await s.async_write_some(as_bytes(payload));
      if (!wr) write_ec = wr.error();

      co_return std::tuple<std::error_code, std::error_code, std::error_code>{connect_ec, read_ec,
                                                                              write_ec};
    }());

  EXPECT_EQ(connect_ec, iocoro::error::not_open);
  EXPECT_EQ(read_ec, iocoro::error::not_open);
  EXPECT_EQ(write_ec, iocoro::error::not_open);
}

TEST(stream_socket_impl_test, not_connected_errors_after_open) {
  iocoro::io_context ctx;
  auto ex = ctx.get_executor();

  stream_socket_impl s{ex};
  auto ec = s.open(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  ASSERT_FALSE(ec) << ec.message();

  std::array<std::byte, 8> tmp{};
  std::string payload = "x";

  auto [read_ec, write_ec] =
    iocoro::sync_wait(ctx, [&]() -> iocoro::awaitable<std::pair<std::error_code, std::error_code>> {
      std::error_code read_ec{};
      std::error_code write_ec{};

      auto rr = co_await s.async_read_some(tmp);
      if (!rr) read_ec = rr.error();

      auto wr = co_await s.async_write_some(as_bytes(payload));
      if (!wr) write_ec = wr.error();

      co_return std::pair<std::error_code, std::error_code>{read_ec, write_ec};
    }());

  EXPECT_EQ(read_ec, iocoro::error::not_connected);
  EXPECT_EQ(write_ec, iocoro::error::not_connected);
}

TEST(stream_socket_impl_test, connect_twice_reports_already_connected) {
  iocoro::io_context ctx;
  auto ex = ctx.get_executor();

  std::uint16_t port = 0;
  auto listen_fd = make_listen_socket_ipv4(port);
  if (!listen_fd) {
    GTEST_SKIP() << "failed to create/bind/listen on 127.0.0.1";
  }

  // Accept in the background so connect can complete.
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

  stream_socket_impl s{ex};
  auto ec = s.open(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  ASSERT_FALSE(ec) << ec.message();

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  (void)::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

  std::error_code ec1{};
  std::error_code ec2{};

  std::tie(ec1, ec2) =
    iocoro::sync_wait(ctx, [&]() -> iocoro::awaitable<std::pair<std::error_code, std::error_code>> {
      auto ec1 = co_await s.async_connect(reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
      auto ec2 = co_await s.async_connect(reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
      co_return std::pair<std::error_code, std::error_code>{ec1, ec2};
    }());

  EXPECT_FALSE(ec1) << ec1.message();
  EXPECT_EQ(ec2, iocoro::error::already_connected);

  // Ensure accept completes and join thread.
  (void)accepted_f.get();
  accept_thread.join();
}

TEST(stream_socket_impl_test, concurrent_read_reports_busy) {
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

  stream_socket_impl s{ex};
  auto ec = s.open(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  ASSERT_FALSE(ec) << ec.message();

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  (void)::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

  std::atomic<bool> started_first{false};

  std::array<std::byte, 1> b1{};
  std::array<std::byte, 1> b2{};

  auto [connect_ec, second_ec] = iocoro::sync_wait_for(
    ctx, 200ms, [&]() -> iocoro::awaitable<std::pair<std::error_code, std::error_code>> {
      auto connect_ec = co_await s.async_connect(reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
      if (connect_ec) {
        co_return std::pair<std::error_code, std::error_code>{connect_ec, std::error_code{}};
      }

      // First read: should block (no data sent).
      auto first_read = iocoro::co_spawn(
        ex,
        [&]() -> iocoro::awaitable<void> {
          started_first.store(true, std::memory_order_release);
          (void)co_await s.async_read_some(b1);
        },
        iocoro::use_awaitable);

      // Wait until the first read has started (yielding to the IO executor).
      for (int i = 0; i < 50 && !started_first.load(std::memory_order_acquire); ++i) {
        (void)co_await iocoro::co_sleep(1ms);
      }
      (void)co_await iocoro::co_sleep(5ms);  // give it a chance to reach the readiness wait

      std::error_code second_ec{};
      auto r2 = co_await s.async_read_some(b2);
      if (!r2) second_ec = r2.error();

      s.close();  // clean up the first read by closing the socket
      try {
        co_await std::move(first_read);
      } catch (...) {
        // ignore cancellation errors
      }

      co_return std::pair<std::error_code, std::error_code>{connect_ec, second_ec};
    }());

  EXPECT_FALSE(connect_ec) << connect_ec.message();
  EXPECT_EQ(second_ec, iocoro::error::busy);

  (void)accepted_f.get();
  accept_thread.join();
}

TEST(stream_socket_impl_test, shutdown_read_and_write_edges) {
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

  stream_socket_impl s{ex};
  auto ec = s.open(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  ASSERT_FALSE(ec) << ec.message();

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  (void)::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

  std::array<std::byte, 1> buf{};
  std::string payload = "x";

  auto [connect_ec, shut_r_ec, shut_w_ec, read_n, write_ec] = iocoro::sync_wait(
    ctx,
    [&]() -> iocoro::awaitable<std::tuple<std::error_code, std::error_code, std::error_code,
                                          std::size_t, std::error_code>> {
      using result_t =
        std::tuple<std::error_code, std::error_code, std::error_code, std::size_t, std::error_code>;

      auto connect_ec = co_await s.async_connect(reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
      if (connect_ec) {
        co_return result_t{connect_ec, std::error_code{}, std::error_code{}, std::size_t{123},
                           std::error_code{}};
      }

      // Empty buffers succeed with 0.
      auto r0 = co_await s.async_read_some(std::span<std::byte>{});
      if (!r0)
        co_return result_t{connect_ec, std::error_code{}, std::error_code{}, std::size_t{123},
                           r0.error()};
      EXPECT_EQ(*r0, 0U);

      auto w0 = co_await s.async_write_some(std::span<std::byte const>{});
      if (!w0)
        co_return result_t{connect_ec, std::error_code{}, std::error_code{}, std::size_t{123},
                           w0.error()};
      EXPECT_EQ(*w0, 0U);

      auto shut_r_ec = s.shutdown(iocoro::shutdown_type::receive);
      auto shut_w_ec = s.shutdown(iocoro::shutdown_type::send);

      auto rr = co_await s.async_read_some(buf);
      if (!rr) co_return result_t{connect_ec, shut_r_ec, shut_w_ec, std::size_t{123}, rr.error()};
      auto const read_n = *rr;  // should be 0 due to read shutdown

      std::error_code write_ec{};
      auto wr = co_await s.async_write_some(as_bytes(payload));
      if (!wr) write_ec = wr.error();

      co_return result_t{connect_ec, shut_r_ec, shut_w_ec, read_n, write_ec};
    }());

  EXPECT_FALSE(connect_ec) << connect_ec.message();
  EXPECT_FALSE(shut_r_ec) << shut_r_ec.message();
  EXPECT_FALSE(shut_w_ec) << shut_w_ec.message();
  EXPECT_EQ(read_n, 0U);
  EXPECT_EQ(write_ec, iocoro::error::broken_pipe);

  (void)accepted_f.get();
  accept_thread.join();
}

TEST(stream_socket_impl_test, redis_ping_ipv4) {
  iocoro::io_context ctx;
  auto ex = ctx.get_executor();

  auto sock = std::make_shared<stream_socket_impl>(ex);
  auto ec = sock->open(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (ec) {
    GTEST_SKIP() << "open(AF_INET, SOCK_STREAM) failed: " << ec.message();
  }

  auto ep_r = iocoro::ip::tcp::endpoint::from_string("127.0.0.1:6379");
  ASSERT_TRUE(ep_r.has_value()) << ep_r.error().message();
  auto const ep = *ep_r;

  auto rr = iocoro::sync_wait_for(
    ctx, 1s, [sock, ep]() -> iocoro::awaitable<iocoro::expected<std::string, std::error_code>> {
      auto ec = co_await sock->async_connect(ep.data(), ep.size());
      if (ec) co_return iocoro::unexpected(ec);

      std::string cmd = "*1\r\n$4\r\nPING\r\n";
      auto wr = co_await iocoro::io::async_write(*sock, as_bytes(cmd));
      if (!wr) co_return iocoro::unexpected(wr.error());

      std::array<std::byte, 4096> buf{};
      auto n = co_await iocoro::io::async_read_until(*sock, std::span{buf}, "\r\n");
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

TEST(stream_socket_impl_test, redis_ping_ipv6) {
  iocoro::io_context ctx;
  auto ex = ctx.get_executor();

  auto sock = std::make_shared<stream_socket_impl>(ex);
  auto ec = sock->open(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
  if (ec) {
    GTEST_SKIP() << "open(AF_INET6, SOCK_STREAM) failed: " << ec.message();
  }

  auto ep_r = iocoro::ip::tcp::endpoint::from_string("[::1]:6379");
  ASSERT_TRUE(ep_r.has_value()) << ep_r.error().message();
  auto const ep = *ep_r;

  auto rr = iocoro::sync_wait_for(
    ctx, 1s, [sock, ep]() -> iocoro::awaitable<iocoro::expected<std::string, std::error_code>> {
      auto ec = co_await sock->async_connect(ep.data(), ep.size());
      if (ec) co_return iocoro::unexpected(ec);

      std::string cmd = "*1\r\n$4\r\nPING\r\n";
      auto wr = co_await iocoro::io::async_write(*sock, as_bytes(cmd));
      if (!wr) co_return iocoro::unexpected(wr.error());

      std::array<std::byte, 4096> buf{};
      auto n = co_await iocoro::io::async_read_until(*sock, std::span{buf}, "\r\n");
      if (!n) co_return iocoro::unexpected(n.error());
      co_return std::string{reinterpret_cast<char const*>(buf.data()), *n};
    }());

  if (!rr && should_skip_net_error(rr.error())) {
    GTEST_SKIP() << "redis not reachable on [::1]:6379: " << rr.error().message();
  }
  ASSERT_TRUE(rr) << rr.error().message();
  ASSERT_GE(rr->size(), 5U);
  EXPECT_TRUE(rr->rfind("+PONG", 0) == 0) << *rr;
}

}  // namespace

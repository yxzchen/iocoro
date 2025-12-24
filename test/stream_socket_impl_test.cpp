#include <gtest/gtest.h>

#include <iocoro/co_sleep.hpp>
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
#include <utility>
#include <future>
#include <thread>
#include <string>
#include <string_view>

#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

using namespace std::chrono_literals;

using iocoro::detail::socket::stream_socket_impl;

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
      co_return iocoro::unexpected(r.error());
    }
    auto n = *r;
    if (n == 0) {
      break;
    }
    out.append(reinterpret_cast<char const*>(tmp.data()), n);
    if (out.size() > 4096) {
      co_return iocoro::unexpected(iocoro::error::invalid_argument);
    }
  }
  co_return out;
}

static auto should_skip_net_error(std::error_code ec) -> bool {
  return ec == std::errc::connection_refused || ec == std::errc::network_unreachable ||
         ec == std::errc::host_unreachable || ec == std::errc::address_not_available;
}

TEST(stream_socket_impl_test, not_open_errors) {
  iocoro::io_context ctx;
  auto ex = ctx.get_executor();

  stream_socket_impl s{ex};

  std::error_code connect_ec{};
  std::error_code read_ec{};
  std::error_code write_ec{};

  std::array<std::byte, 8> tmp{};
  std::string payload = "x";

  iocoro::co_spawn(
    ex,
    [&]() -> iocoro::awaitable<void> {
      sockaddr_in dummy{};
      dummy.sin_family = AF_INET;
      dummy.sin_port = htons(1);
      dummy.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

      connect_ec = co_await s.async_connect(reinterpret_cast<sockaddr*>(&dummy), sizeof(dummy));

      auto rr = co_await s.async_read_some(tmp);
      if (!rr) read_ec = rr.error();

      auto wr = co_await s.async_write_some(as_bytes(payload));
      if (!wr) write_ec = wr.error();
    },
    iocoro::detached);

  (void)ctx.run();
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

  std::error_code read_ec{};
  std::error_code write_ec{};

  iocoro::co_spawn(
    ex,
    [&]() -> iocoro::awaitable<void> {
      auto rr = co_await s.async_read_some(tmp);
      if (!rr) read_ec = rr.error();
      auto wr = co_await s.async_write_some(as_bytes(payload));
      if (!wr) write_ec = wr.error();
    },
    iocoro::detached);

  (void)ctx.run();
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

  iocoro::co_spawn(
    ex,
    [&]() -> iocoro::awaitable<void> {
      ec1 = co_await s.async_connect(reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
      ec2 = co_await s.async_connect(reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    },
    iocoro::detached);

  (void)ctx.run();
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
  std::error_code connect_ec{};
  std::error_code second_ec{};

  std::array<std::byte, 1> b1{};
  std::array<std::byte, 1> b2{};

  iocoro::co_spawn(
    ex,
    [&]() -> iocoro::awaitable<void> {
      connect_ec = co_await s.async_connect(reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
      if (connect_ec) {
        co_return;
      }

      // First read: should block (no data sent).
      iocoro::co_spawn(
        ex,
        [&]() -> iocoro::awaitable<void> {
          started_first.store(true, std::memory_order_release);
          (void)co_await s.async_read_some(b1);
        },
        iocoro::detached);

      // Wait until the first read has started (yielding to the executor).
      for (int i = 0; i < 50 && !started_first.load(std::memory_order_acquire); ++i) {
        (void)co_await iocoro::co_sleep(1ms);
      }
      (void)co_await iocoro::co_sleep(5ms);  // give it a chance to reach the readiness wait

      auto r2 = co_await s.async_read_some(b2);
      if (!r2) second_ec = r2.error();
      s.cancel();  // clean up the first read
    },
    iocoro::detached);

  (void)ctx.run_for(200ms);
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

  std::error_code connect_ec{};
  std::error_code shut_r_ec{};
  std::error_code shut_w_ec{};
  std::size_t read_n = 123;
  std::error_code write_ec{};

  std::array<std::byte, 1> buf{};
  std::string payload = "x";

  iocoro::co_spawn(
    ex,
    [&]() -> iocoro::awaitable<void> {
      connect_ec = co_await s.async_connect(reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
      if (connect_ec) {
        co_return;
      }

      // Empty buffers succeed with 0.
      auto r0 = co_await s.async_read_some(std::span<std::byte>{});
      if (!r0) {
        write_ec = r0.error();
        co_return;
      }
      EXPECT_EQ(*r0, 0U);
      auto w0 = co_await s.async_write_some(std::span<std::byte const>{});
      if (!w0) {
        write_ec = w0.error();
        co_return;
      }
      EXPECT_EQ(*w0, 0U);

      shut_r_ec = s.shutdown(iocoro::shutdown_type::read);
      shut_w_ec = s.shutdown(iocoro::shutdown_type::write);

      auto rr = co_await s.async_read_some(buf);
      if (!rr) {
        write_ec = rr.error();
        co_return;
      }
      read_n = *rr;  // should be 0 due to read shutdown

      auto wr = co_await s.async_write_some(as_bytes(payload));
      if (!wr) write_ec = wr.error();
    },
    iocoro::detached);

  (void)ctx.run();
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
    },
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
    },
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

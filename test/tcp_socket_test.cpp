#include <gtest/gtest.h>

#include <iocoro/co_spawn.hpp>
#include <iocoro/error.hpp>
#include <iocoro/io/read.hpp>
#include <iocoro/io/write.hpp>
#include <iocoro/io_context.hpp>
#include <iocoro/ip/tcp.hpp>
#include <iocoro/steady_timer.hpp>
#include <iocoro/this_coro.hpp>

#include "test_util.hpp"

#include <array>
#include <chrono>
#include <cstring>
#include <thread>

TEST(tcp_socket_test, connect_and_exchange_data) {
  auto [listen_fd, port] = iocoro::test::make_listen_socket_ipv4();
  ASSERT_GE(listen_fd.get(), 0);
  ASSERT_NE(port, 0);

  std::thread server([fd = listen_fd.get()] {
    int client = ::accept(fd, nullptr, nullptr);
    if (client < 0) {
      return;
    }

    std::array<char, 4> buf{};
    std::size_t read_total = 0;
    while (read_total < buf.size()) {
      auto n = ::recv(client, buf.data() + read_total, buf.size() - read_total, 0);
      if (n <= 0) {
        (void)::close(client);
        return;
      }
      read_total += static_cast<std::size_t>(n);
    }

    (void)::send(client, "pong", 4, 0);
    (void)::close(client);
  });

  iocoro::io_context ctx;
  iocoro::ip::tcp::socket sock{ctx};
  iocoro::ip::tcp::endpoint ep{iocoro::ip::address_v4::loopback(), port};

  auto r = iocoro::test::sync_wait(ctx, [&]() -> iocoro::awaitable<iocoro::result<std::size_t>> {
    auto cr = co_await sock.async_connect(ep);
    if (!cr) {
      co_return iocoro::unexpected(cr.error());
    }

    std::array<std::byte, 4> out{};
    std::memcpy(out.data(), "ping", out.size());
    auto wr = co_await iocoro::io::async_write(sock, std::span<std::byte const>{out});
    if (!wr) {
      co_return iocoro::unexpected(wr.error());
    }

    std::array<std::byte, 4> in{};
    auto rd = co_await iocoro::io::async_read(sock, std::span{in});
    if (!rd) {
      co_return iocoro::unexpected(rd.error());
    }

    co_return *rd;
  }());

  server.join();

  ASSERT_TRUE(r);
  ASSERT_TRUE(*r);
  EXPECT_EQ(**r, 4U);
}

TEST(tcp_socket_test, connect_to_closed_port_returns_error) {
  auto [listen_fd, port] = iocoro::test::make_listen_socket_ipv4();
  ASSERT_GE(listen_fd.get(), 0);
  ASSERT_NE(port, 0);
  listen_fd.reset();

  iocoro::io_context ctx;
  iocoro::ip::tcp::socket sock{ctx};
  iocoro::ip::tcp::endpoint ep{iocoro::ip::address_v4::loopback(), port};

  auto r = iocoro::test::sync_wait(ctx, [&]() -> iocoro::awaitable<iocoro::result<void>> {
    co_return co_await sock.async_connect(ep);
  }());

  ASSERT_TRUE(r);
  EXPECT_FALSE(static_cast<bool>(*r));
}

TEST(tcp_socket_test, stress_close_while_read_pending_aborts_without_hang) {
  auto [listen_fd, port] = iocoro::test::make_listen_socket_ipv4();
  ASSERT_GE(listen_fd.get(), 0);
  ASSERT_NE(port, 0);

  std::thread server([fd = listen_fd.get()] {
    int client = ::accept(fd, nullptr, nullptr);
    if (client < 0) {
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{200});
    (void)::close(client);
  });

  iocoro::io_context ctx;
  iocoro::ip::tcp::socket sock{ctx};
  iocoro::ip::tcp::endpoint ep{iocoro::ip::address_v4::loopback(), port};

  auto r = iocoro::test::sync_wait(ctx, [&]() -> iocoro::awaitable<iocoro::result<void>> {
    auto cr = co_await sock.async_connect(ep);
    if (!cr) {
      co_return iocoro::unexpected(cr.error());
    }

    auto ex = co_await iocoro::this_coro::io_executor;
    iocoro::steady_timer timer{ex};
    timer.expires_after(std::chrono::milliseconds{1});

    iocoro::co_spawn(
      ex,
      [&]() -> iocoro::awaitable<void> {
        (void)co_await timer.async_wait(iocoro::use_awaitable);
        sock.close();
        co_return;
      },
      iocoro::detached);

    std::array<std::byte, 256> buf{};
    auto rr = co_await sock.async_read_some(std::span{buf});
    EXPECT_FALSE(static_cast<bool>(rr));
    co_return iocoro::ok();
  }());

  server.join();

  ASSERT_TRUE(r);
  ASSERT_TRUE(*r);
}

TEST(tcp_socket_test, async_read_some_returns_zero_on_peer_graceful_close) {
  auto [listen_fd, port] = iocoro::test::make_listen_socket_ipv4();
  ASSERT_GE(listen_fd.get(), 0);
  ASSERT_NE(port, 0);

  std::thread server([fd = listen_fd.get()] {
    int client = ::accept(fd, nullptr, nullptr);
    if (client < 0) {
      return;
    }
    (void)::close(client);
  });

  iocoro::io_context ctx;
  iocoro::ip::tcp::socket sock{ctx};
  iocoro::ip::tcp::endpoint ep{iocoro::ip::address_v4::loopback(), port};

  auto r = iocoro::test::sync_wait(ctx, [&]() -> iocoro::awaitable<iocoro::result<std::size_t>> {
    auto cr = co_await sock.async_connect(ep);
    if (!cr) {
      co_return iocoro::unexpected(cr.error());
    }

    std::array<std::byte, 1> buf{};
    auto rr = co_await sock.async_read_some(std::span{buf});
    if (!rr) {
      co_return iocoro::unexpected(rr.error());
    }
    co_return *rr;
  }());

  server.join();

  ASSERT_TRUE(r);
  ASSERT_TRUE(*r);
  EXPECT_EQ(**r, 0U);
}

TEST(tcp_socket_test, io_async_read_returns_error_eof_on_peer_graceful_close) {
  auto [listen_fd, port] = iocoro::test::make_listen_socket_ipv4();
  ASSERT_GE(listen_fd.get(), 0);
  ASSERT_NE(port, 0);

  std::thread server([fd = listen_fd.get()] {
    int client = ::accept(fd, nullptr, nullptr);
    if (client < 0) {
      return;
    }
    (void)::close(client);
  });

  iocoro::io_context ctx;
  iocoro::ip::tcp::socket sock{ctx};
  iocoro::ip::tcp::endpoint ep{iocoro::ip::address_v4::loopback(), port};

  auto r = iocoro::test::sync_wait(ctx, [&]() -> iocoro::awaitable<iocoro::result<void>> {
    auto cr = co_await sock.async_connect(ep);
    if (!cr) {
      co_return iocoro::unexpected(cr.error());
    }

    std::array<std::byte, 1> buf{};
    auto rd = co_await iocoro::io::async_read(sock, std::span{buf});
    if (rd) {
      co_return iocoro::unexpected(iocoro::error::internal_error);
    }
    EXPECT_EQ(rd.error(), iocoro::error::eof);
    co_return iocoro::ok();
  }());

  server.join();

  ASSERT_TRUE(r);
  ASSERT_TRUE(*r);
}

TEST(tcp_socket_test, shutdown_receive_makes_future_reads_return_zero_immediately) {
  auto [listen_fd, port] = iocoro::test::make_listen_socket_ipv4();
  ASSERT_GE(listen_fd.get(), 0);
  ASSERT_NE(port, 0);

  std::thread server([fd = listen_fd.get()] {
    int client = ::accept(fd, nullptr, nullptr);
    if (client < 0) {
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{200});
    (void)::close(client);
  });

  iocoro::io_context ctx;
  iocoro::ip::tcp::socket sock{ctx};
  iocoro::ip::tcp::endpoint ep{iocoro::ip::address_v4::loopback(), port};

  auto r = iocoro::test::sync_wait(ctx, [&]() -> iocoro::awaitable<iocoro::result<std::size_t>> {
    auto cr = co_await sock.async_connect(ep);
    if (!cr) {
      co_return iocoro::unexpected(cr.error());
    }

    auto sr = sock.shutdown(iocoro::shutdown_type::receive);
    if (!sr) {
      co_return iocoro::unexpected(sr.error());
    }

    std::array<std::byte, 1> buf{};
    auto rr = co_await sock.async_read_some(std::span{buf});
    if (!rr) {
      co_return iocoro::unexpected(rr.error());
    }
    co_return *rr;
  }());

  server.join();

  ASSERT_TRUE(r);
  ASSERT_TRUE(*r);
  EXPECT_EQ(**r, 0U);
}

TEST(tcp_socket_test, shutdown_send_makes_future_writes_fail_broken_pipe) {
  auto [listen_fd, port] = iocoro::test::make_listen_socket_ipv4();
  ASSERT_GE(listen_fd.get(), 0);
  ASSERT_NE(port, 0);

  std::thread server([fd = listen_fd.get()] {
    int client = ::accept(fd, nullptr, nullptr);
    if (client < 0) {
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{200});
    (void)::close(client);
  });

  iocoro::io_context ctx;
  iocoro::ip::tcp::socket sock{ctx};
  iocoro::ip::tcp::endpoint ep{iocoro::ip::address_v4::loopback(), port};

  auto r = iocoro::test::sync_wait(ctx, [&]() -> iocoro::awaitable<iocoro::result<void>> {
    auto cr = co_await sock.async_connect(ep);
    if (!cr) {
      co_return iocoro::unexpected(cr.error());
    }

    auto sr = sock.shutdown(iocoro::shutdown_type::send);
    if (!sr) {
      co_return iocoro::unexpected(sr.error());
    }

    std::array<std::byte, 1> out{std::byte{0x1}};
    auto wr = co_await sock.async_write_some(std::span<std::byte const>{out});
    if (wr) {
      co_return iocoro::unexpected(iocoro::error::internal_error);
    }
    EXPECT_EQ(wr.error(), iocoro::error::broken_pipe);
    co_return iocoro::ok();
  }());

  server.join();

  ASSERT_TRUE(r);
  ASSERT_TRUE(*r);
}

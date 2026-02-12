#include <gtest/gtest.h>

#include <iocoro/io/read.hpp>
#include <iocoro/io_context.hpp>
#include <iocoro/ip/tcp.hpp>

#include "test_util.hpp"

#include <array>
#include <cstring>
#include <thread>

#include <sys/socket.h>
#include <unistd.h>

TEST(tcp_acceptor_test, open_bind_listen_accept_and_exchange_data) {
  iocoro::io_context ctx;
  iocoro::ip::tcp::acceptor acc{ctx};

  iocoro::ip::tcp::endpoint listen_ep{iocoro::ip::address_v4::loopback(), 0};
  auto lr = acc.listen(listen_ep);
  ASSERT_TRUE(lr) << lr.error().message();

  auto local_ep = acc.local_endpoint();
  ASSERT_TRUE(local_ep);

  std::thread client([ep = *local_ep] {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
      return;
    }
    if (::connect(fd, ep.data(), ep.size()) != 0) {
      (void)::close(fd);
      return;
    }
    (void)::send(fd, "ping", 4, 0);
    (void)::close(fd);
  });

  auto r = iocoro::test::sync_wait(ctx, [&]() -> iocoro::awaitable<iocoro::result<std::size_t>> {
    auto accepted = co_await acc.async_accept();
    if (!accepted) {
      co_return iocoro::unexpected(accepted.error());
    }

    std::array<std::byte, 4> in{};
    auto rd = co_await iocoro::io::async_read(*accepted, std::span{in});
    if (!rd) {
      co_return iocoro::unexpected(rd.error());
    }
    co_return *rd;
  }());

  client.join();

  ASSERT_TRUE(r);
  ASSERT_TRUE(*r);
  EXPECT_EQ(**r, 4U);
}

TEST(tcp_acceptor_test, accepts_multiple_connections_sequentially) {
  iocoro::io_context ctx;
  iocoro::ip::tcp::acceptor acc{ctx};

  iocoro::ip::tcp::endpoint listen_ep{iocoro::ip::address_v4::loopback(), 0};
  auto lr = acc.listen(listen_ep);
  ASSERT_TRUE(lr) << lr.error().message();

  auto local_ep = acc.local_endpoint();
  ASSERT_TRUE(local_ep);

  std::thread client([ep = *local_ep] {
    for (int i = 0; i < 2; ++i) {
      int fd = ::socket(AF_INET, SOCK_STREAM, 0);
      if (fd < 0) {
        return;
      }
      if (::connect(fd, ep.data(), ep.size()) != 0) {
        (void)::close(fd);
        return;
      }
      (void)::send(fd, "ping", 4, 0);
      (void)::close(fd);
    }
  });

  auto r = iocoro::test::sync_wait(ctx, [&]() -> iocoro::awaitable<iocoro::result<std::size_t>> {
    for (int i = 0; i < 2; ++i) {
      auto accepted = co_await acc.async_accept();
      if (!accepted) {
        co_return iocoro::unexpected(accepted.error());
      }
      std::array<std::byte, 4> in{};
      auto rd = co_await iocoro::io::async_read(*accepted, std::span{in});
      if (!rd) {
        co_return iocoro::unexpected(rd.error());
      }
    }
    co_return 4U;
  }());

  client.join();

  ASSERT_TRUE(r);
  ASSERT_TRUE(*r);
  EXPECT_EQ(**r, 4U);
}

TEST(tcp_acceptor_test, listen_with_mismatched_family_on_open_socket_returns_invalid_argument) {
  iocoro::io_context ctx;
  iocoro::ip::tcp::acceptor acc{ctx};

  auto r1 = acc.listen(iocoro::ip::tcp::endpoint{iocoro::ip::address_v4::loopback(), 0});
  ASSERT_TRUE(r1) << r1.error().message();

  auto r2 = acc.listen(iocoro::ip::tcp::endpoint{iocoro::ip::address_v6::loopback(), 0});
  ASSERT_FALSE(r2);
  EXPECT_EQ(r2.error(), iocoro::error::invalid_argument);
}

TEST(tcp_acceptor_test, listen_propagates_configure_failure) {
  iocoro::io_context ctx;
  iocoro::ip::tcp::acceptor acc{ctx};

  auto r = acc.listen(iocoro::ip::tcp::endpoint{iocoro::ip::address_v4::loopback(), 0},
                      0,
                      [](auto&) -> iocoro::result<void> {
                        return iocoro::fail(iocoro::error::invalid_argument);
                      });

  ASSERT_FALSE(r);
  EXPECT_EQ(r.error(), iocoro::error::invalid_argument);
}

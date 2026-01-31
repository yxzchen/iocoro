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
  auto ec = acc.listen(listen_ep);
  ASSERT_FALSE(ec) << ec.message();

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

  auto r = iocoro::test::sync_wait(
    ctx, [&]() -> iocoro::awaitable<iocoro::expected<std::size_t, std::error_code>> {
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

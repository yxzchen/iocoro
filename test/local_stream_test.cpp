#include <gtest/gtest.h>

#include <iocoro/io/read.hpp>
#include <iocoro/io_context.hpp>
#include <iocoro/local/stream.hpp>

#include "test_util.hpp"

#include <array>
#include <cstdio>
#include <cstring>
#include <thread>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

TEST(local_stream_test, accept_and_exchange_data) {
  auto path = iocoro::test::make_temp_path("iocoro_local_stream");
  iocoro::test::unlink_path(path);

  auto ep = iocoro::local::endpoint::from_path(path);
  ASSERT_TRUE(ep);

  iocoro::io_context ctx;
  iocoro::local::stream::acceptor acc{ctx};

  auto ec = acc.listen(*ep);
  ASSERT_FALSE(ec) << ec.message();

  std::thread client([path] {
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
      return;
    }
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path.c_str());
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
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
  iocoro::test::unlink_path(path);

  ASSERT_TRUE(r);
  ASSERT_TRUE(*r);
  EXPECT_EQ(**r, 4U);
}

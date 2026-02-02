#include <gtest/gtest.h>

#include <iocoro/io/read.hpp>
#include <iocoro/io_context.hpp>
#include <iocoro/local/stream.hpp>

#include "test_util.hpp"

#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
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

  auto lr = acc.listen(*ep);
  ASSERT_TRUE(lr) << lr.error().message();

  std::thread client([path] {
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
      return;
    }
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path.c_str());
    bool connected = false;
    for (int i = 0; i < 200; ++i) {
      if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
        connected = true;
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    if (!connected) {
      (void)::close(fd);
      return;
    }
    (void)::send(fd, "ping", 4, 0);
    (void)::close(fd);
  });

  auto r = iocoro::test::sync_wait(
    ctx, [&]() -> iocoro::awaitable<iocoro::result<std::size_t>> {
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

TEST(local_stream_test, endpoint_from_path_rejects_invalid_lengths) {
  auto empty = iocoro::local::endpoint::from_path("");
  ASSERT_FALSE(empty);
  EXPECT_EQ(empty.error(), iocoro::error::invalid_argument);

  std::string long_path(sizeof(sockaddr_un::sun_path), 'a');
  auto too_long = iocoro::local::endpoint::from_path(long_path);
  ASSERT_FALSE(too_long);
  EXPECT_EQ(too_long.error(), iocoro::error::invalid_argument);
}

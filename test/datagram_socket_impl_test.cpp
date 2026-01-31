#include <gtest/gtest.h>

#include <iocoro/detail/socket/datagram_socket_impl.hpp>
#include <iocoro/io_context.hpp>

#include "test_util.hpp"

#include <array>
#include <sys/socket.h>

TEST(datagram_socket_impl_test, receive_without_open_returns_not_open) {
  iocoro::io_context ctx;
  iocoro::detail::socket::datagram_socket_impl impl{ctx.get_executor()};

  std::array<std::byte, 4> buf{};
  sockaddr_storage src{};
  socklen_t len = sizeof(src);
  auto r = iocoro::test::sync_wait(
    ctx, [&]() -> iocoro::awaitable<iocoro::expected<std::size_t, std::error_code>> {
      co_return co_await impl.async_receive_from(std::span{buf},
                                                 reinterpret_cast<sockaddr*>(&src), &len);
    }());

  ASSERT_TRUE(r);
  ASSERT_FALSE(*r);
  EXPECT_EQ(r->error(), iocoro::error::not_open);
}

TEST(datagram_socket_impl_test, receive_without_bind_returns_not_bound) {
  iocoro::io_context ctx;
  iocoro::detail::socket::datagram_socket_impl impl{ctx.get_executor()};

  auto ec = impl.open(AF_INET, SOCK_DGRAM, 0);
  ASSERT_FALSE(ec) << ec.message();

  std::array<std::byte, 4> buf{};
  sockaddr_storage src{};
  socklen_t len = sizeof(src);
  auto r = iocoro::test::sync_wait(
    ctx, [&]() -> iocoro::awaitable<iocoro::expected<std::size_t, std::error_code>> {
      co_return co_await impl.async_receive_from(std::span{buf},
                                                 reinterpret_cast<sockaddr*>(&src), &len);
    }());

  ASSERT_TRUE(r);
  ASSERT_FALSE(*r);
  EXPECT_EQ(r->error(), iocoro::error::not_bound);
}

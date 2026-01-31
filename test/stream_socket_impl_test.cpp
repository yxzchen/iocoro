#include <gtest/gtest.h>

#include <iocoro/detail/socket/stream_socket_impl.hpp>
#include <iocoro/io_context.hpp>

#include "test_util.hpp"

#include <array>
#include <sys/socket.h>

TEST(stream_socket_impl_test, read_without_open_returns_not_open) {
  iocoro::io_context ctx;
  iocoro::detail::socket::stream_socket_impl impl{ctx.get_executor()};

  std::array<std::byte, 4> buf{};
  auto r = iocoro::test::sync_wait(
    ctx, [&]() -> iocoro::awaitable<iocoro::expected<std::size_t, std::error_code>> {
      co_return co_await impl.async_read_some(std::span{buf});
    }());

  ASSERT_TRUE(r);
  ASSERT_FALSE(*r);
  EXPECT_EQ(r->error(), iocoro::error::not_open);
}

TEST(stream_socket_impl_test, read_without_connect_returns_not_connected) {
  iocoro::io_context ctx;
  iocoro::detail::socket::stream_socket_impl impl{ctx.get_executor()};

  auto ec = impl.open(AF_INET, SOCK_STREAM, 0);
  ASSERT_FALSE(ec) << ec.message();

  std::array<std::byte, 4> buf{};
  auto r = iocoro::test::sync_wait(
    ctx, [&]() -> iocoro::awaitable<iocoro::expected<std::size_t, std::error_code>> {
      co_return co_await impl.async_read_some(std::span{buf});
    }());

  ASSERT_TRUE(r);
  ASSERT_FALSE(*r);
  EXPECT_EQ(r->error(), iocoro::error::not_connected);
}

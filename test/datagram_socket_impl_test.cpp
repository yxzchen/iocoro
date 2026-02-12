#include <gtest/gtest.h>

#include <iocoro/detail/socket/datagram_socket_impl.hpp>
#include <iocoro/io_context.hpp>

#include "test_util.hpp"

#include <netinet/in.h>
#include <sys/socket.h>
#include <array>

TEST(datagram_socket_impl_test, receive_without_open_returns_not_open) {
  iocoro::io_context ctx;
  iocoro::detail::socket::datagram_socket_impl impl{ctx.get_executor()};

  std::array<std::byte, 4> buf{};
  sockaddr_storage src{};
  socklen_t len = sizeof(src);
  auto r = iocoro::test::sync_wait(ctx, [&]() -> iocoro::awaitable<iocoro::result<std::size_t>> {
    co_return co_await impl.async_receive_from(std::span{buf}, reinterpret_cast<sockaddr*>(&src),
                                               &len);
  }());

  ASSERT_TRUE(r);
  ASSERT_FALSE(*r);
  EXPECT_EQ(r->error(), iocoro::error::not_open);
}

TEST(datagram_socket_impl_test, receive_without_bind_returns_not_bound) {
  iocoro::io_context ctx;
  iocoro::detail::socket::datagram_socket_impl impl{ctx.get_executor()};

  auto ec = impl.open(AF_INET, SOCK_DGRAM, 0);
  ASSERT_TRUE(ec) << (ec ? "" : ec.error().message());

  std::array<std::byte, 4> buf{};
  sockaddr_storage src{};
  socklen_t len = sizeof(src);
  auto r = iocoro::test::sync_wait(ctx, [&]() -> iocoro::awaitable<iocoro::result<std::size_t>> {
    co_return co_await impl.async_receive_from(std::span{buf}, reinterpret_cast<sockaddr*>(&src),
                                               &len);
  }());

  ASSERT_TRUE(r);
  ASSERT_FALSE(*r);
  EXPECT_EQ(r->error(), iocoro::error::not_bound);
}

TEST(datagram_socket_impl_test, send_empty_buffer_returns_zero) {
  iocoro::io_context ctx;
  iocoro::detail::socket::datagram_socket_impl impl{ctx.get_executor()};

  auto ec = impl.open(AF_INET, SOCK_DGRAM, 0);
  ASSERT_TRUE(ec) << (ec ? "" : ec.error().message());

  sockaddr_in dest{};
  dest.sin_family = AF_INET;
  dest.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  dest.sin_port = htons(0);

  std::array<std::byte, 1> empty{};
  auto r = iocoro::test::sync_wait(ctx, [&]() -> iocoro::awaitable<iocoro::result<std::size_t>> {
    co_return co_await impl.async_send_to(std::span<std::byte const>{empty}.first(0),
                                          reinterpret_cast<sockaddr const*>(&dest), sizeof(dest));
  }());

  ASSERT_TRUE(r);
  ASSERT_TRUE(*r);
  EXPECT_EQ(**r, 0U);
}

TEST(datagram_socket_impl_test, receive_empty_buffer_returns_invalid_argument) {
  iocoro::io_context ctx;
  iocoro::detail::socket::datagram_socket_impl impl{ctx.get_executor()};

  auto ec = impl.open(AF_INET, SOCK_DGRAM, 0);
  ASSERT_TRUE(ec) << (ec ? "" : ec.error().message());

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(0);
  ec = impl.bind(reinterpret_cast<sockaddr const*>(&addr), sizeof(addr));
  ASSERT_TRUE(ec) << (ec ? "" : ec.error().message());

  std::array<std::byte, 1> empty{};
  sockaddr_storage src{};
  socklen_t len = sizeof(src);
  auto r = iocoro::test::sync_wait(ctx, [&]() -> iocoro::awaitable<iocoro::result<std::size_t>> {
    co_return co_await impl.async_receive_from(std::span{empty}.first(0),
                                               reinterpret_cast<sockaddr*>(&src), &len);
  }());

  ASSERT_TRUE(r);
  ASSERT_FALSE(*r);
  EXPECT_EQ(r->error(), iocoro::error::invalid_argument);
}

TEST(datagram_socket_impl_test, connected_send_to_mismatched_destination_returns_invalid_argument) {
  iocoro::io_context ctx;
  iocoro::detail::socket::datagram_socket_impl impl{ctx.get_executor()};

  auto ec = impl.open(AF_INET, SOCK_DGRAM, 0);
  ASSERT_TRUE(ec) << (ec ? "" : ec.error().message());

  sockaddr_in connected{};
  connected.sin_family = AF_INET;
  connected.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  connected.sin_port = htons(10001);
  ec = impl.connect(reinterpret_cast<sockaddr const*>(&connected), sizeof(connected));
  ASSERT_TRUE(ec) << (ec ? "" : ec.error().message());

  sockaddr_in other{};
  other.sin_family = AF_INET;
  other.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  other.sin_port = htons(10002);

  std::array<std::byte, 4> buf{
    std::byte{0xAA},
    std::byte{0xBB},
    std::byte{0xCC},
    std::byte{0xDD},
  };
  auto r = iocoro::test::sync_wait(ctx, [&]() -> iocoro::awaitable<iocoro::result<std::size_t>> {
    co_return co_await impl.async_send_to(std::span<std::byte const>{buf},
                                          reinterpret_cast<sockaddr const*>(&other),
                                          sizeof(other));
  }());

  ASSERT_TRUE(r);
  ASSERT_FALSE(*r);
  EXPECT_EQ(r->error(), iocoro::error::invalid_argument);
}

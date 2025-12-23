#include <gtest/gtest.h>

#include <iocoro/detail/socket/socket_impl_base.hpp>
#include <iocoro/io_context.hpp>
#include <iocoro/socket_option.hpp>
#include <iocoro/src.hpp>

#include <system_error>

#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

using iocoro::detail::socket::socket_impl_base;

TEST(socket_impl_base_test, open_close_is_idempotent) {
  socket_impl_base s;

  auto ec = s.open(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  ASSERT_FALSE(ec) << ec.message();
  EXPECT_TRUE(s.is_open());

  auto fd = s.native_handle();
  ASSERT_GE(fd, 0);
  EXPECT_NE(::fcntl(fd, F_GETFD), -1);

  s.close();
  EXPECT_FALSE(s.is_open());
  EXPECT_EQ(s.native_handle(), -1);
  EXPECT_EQ(::fcntl(fd, F_GETFD), -1);

  // Idempotent close.
  s.close();
  EXPECT_FALSE(s.is_open());
  EXPECT_EQ(s.native_handle(), -1);
}

TEST(socket_impl_base_test, assign_and_release_transfer_ownership) {
  socket_impl_base s;

  int fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  ASSERT_GE(fd, 0);

  auto ec = s.assign(fd);
  ASSERT_FALSE(ec) << ec.message();
  EXPECT_TRUE(s.is_open());
  EXPECT_EQ(s.native_handle(), fd);

  int released = s.release();
  EXPECT_EQ(released, fd);
  EXPECT_FALSE(s.is_open());
  EXPECT_EQ(s.native_handle(), -1);

  // Released fd must remain open; caller is responsible for closing it.
  EXPECT_NE(::fcntl(released, F_GETFD), -1);
  (void)::close(released);
}

TEST(socket_impl_base_test, cancel_does_not_close_fd) {
  iocoro::io_context ctx;
  auto ex = ctx.get_executor();

  socket_impl_base s(ex);
  auto ec = s.open(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  ASSERT_FALSE(ec) << ec.message();

  auto fd = s.native_handle();
  ASSERT_GE(fd, 0);

  s.cancel();
  EXPECT_TRUE(s.is_open());
  EXPECT_NE(::fcntl(fd, F_GETFD), -1);

  s.close();
  EXPECT_FALSE(s.is_open());
  EXPECT_EQ(::fcntl(fd, F_GETFD), -1);
}

TEST(socket_impl_base_test, set_get_option_basic) {
  socket_impl_base s;
  auto ec = s.open(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  ASSERT_FALSE(ec) << ec.message();

  iocoro::socket_option::reuse_address reuse{true};
  ec = s.set_option(reuse);
  ASSERT_FALSE(ec) << ec.message();

  iocoro::socket_option::reuse_address got{};
  ec = s.get_option(got);
  ASSERT_FALSE(ec) << ec.message();

  // On Linux, SO_REUSEADDR is generally readable; value should be 1 after setting.
  EXPECT_TRUE(got.enabled());
}

}  // namespace



#include <gtest/gtest.h>

#include <iocoro/detail/socket/socket_impl_base.hpp>
#include <iocoro/io_context.hpp>

#include <sys/socket.h>
#include <unistd.h>

TEST(socket_impl_base_test, open_close_lifecycle) {
  iocoro::io_context ctx;
  iocoro::detail::socket::socket_impl_base base{ctx.get_executor()};

  auto ec = base.open(AF_INET, SOCK_STREAM, 0);
  ASSERT_TRUE(ec) << (ec ? "" : ec.error().message());
  EXPECT_TRUE(base.is_open());
  EXPECT_GE(base.native_handle(), 0);

  base.close();
  EXPECT_FALSE(base.is_open());
}

TEST(socket_impl_base_test, release_returns_fd_and_closes_registration) {
  iocoro::io_context ctx;
  iocoro::detail::socket::socket_impl_base base{ctx.get_executor()};

  auto ec = base.open(AF_INET, SOCK_STREAM, 0);
  ASSERT_TRUE(ec) << (ec ? "" : ec.error().message());

  auto fd = base.release();
  EXPECT_GE(fd, 0);
  EXPECT_FALSE(base.is_open());
  if (fd >= 0) {
    (void)::close(fd);
  }
}

TEST(socket_impl_base_test, assign_adopts_fd) {
  iocoro::io_context ctx;
  iocoro::detail::socket::socket_impl_base base{ctx.get_executor()};

  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  ASSERT_GE(fd, 0);

  auto ec = base.assign(fd);
  ASSERT_TRUE(ec) << (ec ? "" : ec.error().message());
  EXPECT_TRUE(base.is_open());

  base.close();
  EXPECT_FALSE(base.is_open());
}

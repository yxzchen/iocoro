#include <gtest/gtest.h>

#include <iocoro/detail/socket/socket_impl_base.hpp>
#include <iocoro/io_context.hpp>

#include "test_util.hpp"

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
  ASSERT_TRUE(fd) << fd.error().message();
  EXPECT_GE(*fd, 0);
  EXPECT_FALSE(base.is_open());
  if (*fd >= 0) {
    (void)::close(*fd);
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

TEST(socket_impl_base_test, release_with_inflight_guard_returns_busy) {
  iocoro::io_context ctx;
  iocoro::detail::socket::socket_impl_base base{ctx.get_executor()};

  int fds[2]{-1, -1};
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
  ASSERT_GE(fds[0], 0);
  ASSERT_GE(fds[1], 0);

  auto ec = base.assign(fds[0]);
  ASSERT_TRUE(ec) << (ec ? "" : ec.error().message());

  auto res = base.acquire_resource();
  ASSERT_TRUE(res);
  auto guard = base.make_operation_guard(res);
  ASSERT_TRUE(guard);
  ASSERT_TRUE(base.has_pending_operations());

  auto released = base.release();
  ASSERT_FALSE(released);
  EXPECT_EQ(released.error(), iocoro::error::busy);

  guard.reset();
  auto released_after = base.release();
  ASSERT_TRUE(released_after);
  EXPECT_GE(*released_after, 0);

  (void)::close(*released_after);
  (void)::close(fds[1]);
}

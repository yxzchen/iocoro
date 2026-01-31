#include <gtest/gtest.h>

#include <iocoro/detail/socket/acceptor_impl.hpp>
#include <iocoro/io_context.hpp>

#include "test_util.hpp"

#include <sys/socket.h>

TEST(acceptor_impl_test, async_accept_without_open_returns_not_open) {
  iocoro::io_context ctx;
  iocoro::detail::socket::acceptor_impl acc{ctx.get_executor()};

  auto r = iocoro::test::sync_wait(
    ctx, [&]() -> iocoro::awaitable<iocoro::expected<int, std::error_code>> {
      co_return co_await acc.async_accept();
    }());

  ASSERT_TRUE(r);
  ASSERT_FALSE(*r);
  EXPECT_EQ(r->error(), iocoro::error::not_open);
}

TEST(acceptor_impl_test, async_accept_without_listen_returns_not_listening) {
  iocoro::io_context ctx;
  iocoro::detail::socket::acceptor_impl acc{ctx.get_executor()};

  auto ec = acc.open(AF_INET, SOCK_STREAM, 0);
  ASSERT_FALSE(ec) << ec.message();

  auto r = iocoro::test::sync_wait(
    ctx, [&]() -> iocoro::awaitable<iocoro::expected<int, std::error_code>> {
      co_return co_await acc.async_accept();
    }());

  ASSERT_TRUE(r);
  ASSERT_FALSE(*r);
  EXPECT_EQ(r->error(), iocoro::error::not_listening);
}

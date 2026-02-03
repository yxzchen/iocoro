#include <gtest/gtest.h>

#include <iocoro/detail/socket/acceptor_impl.hpp>
#include <iocoro/io_context.hpp>

#include "test_util.hpp"

#include <netinet/in.h>
#include <sys/socket.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>

TEST(acceptor_impl_test, async_accept_without_open_returns_not_open) {
  iocoro::io_context ctx;
  iocoro::detail::socket::acceptor_impl acc{ctx.get_executor()};

  auto r = iocoro::test::sync_wait(ctx, [&]() -> iocoro::awaitable<iocoro::result<int>> {
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
  ASSERT_TRUE(ec) << (ec ? "" : ec.error().message());

  auto r = iocoro::test::sync_wait(ctx, [&]() -> iocoro::awaitable<iocoro::result<int>> {
    co_return co_await acc.async_accept();
  }());

  ASSERT_TRUE(r);
  ASSERT_FALSE(*r);
  EXPECT_EQ(r->error(), iocoro::error::not_listening);
}

TEST(acceptor_impl_test, cancel_read_aborts_pending_accept) {
  iocoro::io_context ctx;
  iocoro::detail::socket::acceptor_impl acc{ctx.get_executor()};

  auto ec = acc.open(AF_INET, SOCK_STREAM, 0);
  ASSERT_TRUE(ec) << (ec ? "" : ec.error().message());

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(0);
  ec = acc.bind(reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
  ASSERT_TRUE(ec) << (ec ? "" : ec.error().message());

  ec = acc.listen(16);
  ASSERT_TRUE(ec) << (ec ? "" : ec.error().message());

  std::mutex m;
  std::condition_variable cv;
  std::atomic<bool> done{false};
  std::optional<iocoro::expected<iocoro::result<int>, std::exception_ptr>> result;

  iocoro::co_spawn(
    ctx.get_executor(),
    [&]() -> iocoro::awaitable<iocoro::result<int>> { co_return co_await acc.async_accept(); },
    [&](iocoro::expected<iocoro::result<int>, std::exception_ptr> r) {
      result = std::move(r);
      std::scoped_lock lk{m};
      done.store(true);
      cv.notify_all();
    });

  (void)ctx.run_for(std::chrono::milliseconds{1});
  acc.cancel_read();
  ctx.run();

  std::unique_lock lk{m};
  cv.wait(lk, [&] { return done.load(); });

  ASSERT_TRUE(result);
  ASSERT_TRUE(*result);
  ASSERT_FALSE(**result);
  EXPECT_EQ(result->value().error(), iocoro::error::operation_aborted);
}

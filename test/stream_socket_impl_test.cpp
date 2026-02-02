#include <gtest/gtest.h>

#include <iocoro/detail/socket/stream_socket_impl.hpp>
#include <iocoro/io_context.hpp>

#include "test_util.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <sys/socket.h>
#include <unistd.h>

TEST(stream_socket_impl_test, read_without_open_returns_not_open) {
  iocoro::io_context ctx;
  iocoro::detail::socket::stream_socket_impl impl{ctx.get_executor()};

  std::array<std::byte, 4> buf{};
  auto r = iocoro::test::sync_wait(
    ctx, [&]() -> iocoro::awaitable<iocoro::result<std::size_t>> {
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
  ASSERT_TRUE(ec) << (ec ? "" : ec.error().message());

  std::array<std::byte, 4> buf{};
  auto r = iocoro::test::sync_wait(
    ctx, [&]() -> iocoro::awaitable<iocoro::result<std::size_t>> {
      co_return co_await impl.async_read_some(std::span{buf});
    }());

  ASSERT_TRUE(r);
  ASSERT_FALSE(*r);
  EXPECT_EQ(r->error(), iocoro::error::not_connected);
}

TEST(stream_socket_impl_test, concurrent_reads_return_busy_and_cancel_aborts) {
  iocoro::io_context ctx;
  iocoro::detail::socket::stream_socket_impl impl{ctx.get_executor()};

  int fds[2]{-1, -1};
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
  ASSERT_GE(fds[0], 0);
  ASSERT_GE(fds[1], 0);

  auto ec = impl.assign(fds[0]);
  ASSERT_TRUE(ec) << (ec ? "" : ec.error().message());
  int peer = fds[1];

  std::array<std::byte, 4> buf1{};
  std::array<std::byte, 4> buf2{};
  std::mutex m;
  std::condition_variable cv;
  std::atomic<int> done{0};
  std::optional<iocoro::expected<iocoro::result<std::size_t>, std::exception_ptr>> r1;
  std::optional<iocoro::expected<iocoro::result<std::size_t>, std::exception_ptr>> r2;

  auto on_done =
    [&](auto& slot, iocoro::expected<iocoro::result<std::size_t>, std::exception_ptr> r) {
    slot = std::move(r);
    if (done.fetch_add(1) + 1 == 2) {
      std::scoped_lock lk{m};
      cv.notify_all();
    }
  };

  auto ex = ctx.get_executor();
  iocoro::co_spawn(
    ex,
    [&]() -> iocoro::awaitable<iocoro::result<std::size_t>> {
      co_return co_await impl.async_read_some(std::span{buf1});
    },
    [&](iocoro::expected<iocoro::result<std::size_t>, std::exception_ptr> r) {
      on_done(r1, std::move(r));
    });

  iocoro::co_spawn(
    ex,
    [&]() -> iocoro::awaitable<iocoro::result<std::size_t>> {
      co_return co_await impl.async_read_some(std::span{buf2});
    },
    [&](iocoro::expected<iocoro::result<std::size_t>, std::exception_ptr> r) {
      on_done(r2, std::move(r));
    });

  (void)ctx.run_for(std::chrono::milliseconds{1});
  impl.cancel_read();
  ctx.run();

  std::unique_lock lk{m};
  cv.wait(lk, [&] { return done.load() == 2; });

  ASSERT_TRUE(r1);
  ASSERT_TRUE(r2);
  ASSERT_TRUE(*r1);
  ASSERT_TRUE(*r2);
  ASSERT_FALSE(**r1);
  EXPECT_EQ(r1->value().error(), iocoro::error::operation_aborted);
  ASSERT_FALSE(**r2);
  EXPECT_EQ(r2->value().error(), iocoro::error::busy);

  (void)::close(peer);
}

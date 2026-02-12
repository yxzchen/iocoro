#include <gtest/gtest.h>

#include <iocoro/detail/socket/socket_impl_base.hpp>
#include <iocoro/io_context.hpp>

#include "test_util.hpp"

#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>

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

TEST(socket_impl_base_test, release_with_inflight_wait_returns_busy) {
  iocoro::io_context ctx;
  iocoro::detail::socket::socket_impl_base base{ctx.get_executor()};

  int fds[2]{-1, -1};
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
  ASSERT_GE(fds[0], 0);
  ASSERT_GE(fds[1], 0);

  auto ec = base.assign(fds[0]);
  ASSERT_TRUE(ec) << (ec ? "" : ec.error().message());

  int peer = fds[1];
  std::mutex m;
  std::condition_variable cv;
  std::atomic<bool> done{false};
  std::optional<iocoro::expected<iocoro::result<void>, std::exception_ptr>> wait_result;

  iocoro::co_spawn(
    ctx.get_executor(), [&]() -> iocoro::awaitable<iocoro::result<void>> {
      co_return co_await base.wait_read_ready();
    },
    [&](iocoro::expected<iocoro::result<void>, std::exception_ptr> r) {
      wait_result = std::move(r);
      std::scoped_lock lk{m};
      done.store(true);
      cv.notify_all();
    });

  auto const deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{200};
  while (!base.has_pending_operations() && !done.load(std::memory_order_acquire) &&
         std::chrono::steady_clock::now() < deadline) {
    (void)ctx.run_for(std::chrono::milliseconds{1});
  }

  ASSERT_TRUE(base.has_pending_operations()) << "wait_read_ready did not become in-flight in time";

  auto released = base.release();
  ASSERT_FALSE(released);
  EXPECT_EQ(released.error(), iocoro::error::busy);

  base.cancel_read();
  ctx.run();

  std::unique_lock lk{m};
  cv.wait(lk, [&] { return done.load(); });

  ASSERT_TRUE(wait_result);
  ASSERT_TRUE(*wait_result);
  ASSERT_FALSE(**wait_result);
  EXPECT_EQ(wait_result->value().error(), iocoro::error::operation_aborted);

  (void)::close(peer);
}

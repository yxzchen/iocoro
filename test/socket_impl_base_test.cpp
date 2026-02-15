#include <gtest/gtest.h>

#include <iocoro/detail/socket/socket_impl_base.hpp>
#include <iocoro/io_context.hpp>

#include "test_util.hpp"

#include <sys/socket.h>
#include <unistd.h>
#include <atomic>
#include <chrono>
#include <thread>

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

TEST(socket_impl_base_test, release_never_succeeds_while_operation_guard_is_live_under_race) {
  iocoro::io_context ctx;
  iocoro::detail::socket::socket_impl_base base{ctx.get_executor()};

  int fds[2]{-1, -1};
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
  ASSERT_GE(fds[0], 0);
  ASSERT_GE(fds[1], 0);

  auto ec = base.assign(fds[0]);
  ASSERT_TRUE(ec) << (ec ? "" : ec.error().message());

  std::atomic<bool> stop{false};
  std::atomic<int> live_guards{0};
  std::atomic<int> acquired_guards{0};

  std::thread worker([&] {
    while (!stop.load(std::memory_order_acquire)) {
      auto res = base.acquire_resource();
      if (!res) {
        break;
      }
      auto guard = base.make_operation_guard(res);
      if (!guard) {
        std::this_thread::yield();
        continue;
      }
      live_guards.fetch_add(1, std::memory_order_acq_rel);
      acquired_guards.fetch_add(1, std::memory_order_relaxed);
      std::this_thread::sleep_for(std::chrono::microseconds{50});
      live_guards.fetch_sub(1, std::memory_order_acq_rel);
    }
  });

  int released_fd = -1;
  auto const started_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{500};
  while (acquired_guards.load(std::memory_order_relaxed) == 0 &&
         std::chrono::steady_clock::now() < started_deadline) {
    std::this_thread::yield();
  }

  auto const race_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{500};
  while (std::chrono::steady_clock::now() < race_deadline) {
    auto released = base.release();
    if (released) {
      EXPECT_EQ(live_guards.load(std::memory_order_acquire), 0);
      released_fd = *released;
      break;
    }
    EXPECT_EQ(released.error(), iocoro::error::busy);
    std::this_thread::yield();
  }

  stop.store(true, std::memory_order_release);
  worker.join();

  EXPECT_GT(acquired_guards.load(std::memory_order_relaxed), 0);
  if (released_fd < 0) {
    auto released_after = base.release();
    ASSERT_TRUE(released_after);
    released_fd = *released_after;
  }
  ASSERT_GE(released_fd, 0);

  (void)::close(released_fd);
  (void)::close(fds[1]);
}

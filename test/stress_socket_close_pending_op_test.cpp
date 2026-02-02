#include <gtest/gtest.h>

#include <iocoro/co_spawn.hpp>
#include <iocoro/error.hpp>
#include <iocoro/io_context.hpp>
#include <iocoro/ip/tcp.hpp>
#include <iocoro/steady_timer.hpp>
#include <iocoro/this_coro.hpp>

#include "test_util.hpp"

#include <array>
#include <chrono>
#include <thread>

using namespace std::chrono_literals;

TEST(stress_socket_close_pending_op, close_while_read_pending_aborts_without_hang) {
  auto [listen_fd, port] = iocoro::test::make_listen_socket_ipv4();
  ASSERT_GE(listen_fd.get(), 0);
  ASSERT_NE(port, 0);

  std::thread server([fd = listen_fd.get()] {
    int client = ::accept(fd, nullptr, nullptr);
    if (client < 0) {
      return;
    }
    std::this_thread::sleep_for(200ms);
    (void)::close(client);
  });

  iocoro::io_context ctx;
  iocoro::ip::tcp::socket sock{ctx};
  iocoro::ip::tcp::endpoint ep{iocoro::ip::address_v4::loopback(), port};

  auto r = iocoro::test::sync_wait(
    ctx, [&]() -> iocoro::awaitable<iocoro::result<void>> {
      auto cr = co_await sock.async_connect(ep);
      if (!cr) {
        co_return iocoro::unexpected(cr.error());
      }

      auto ex = co_await iocoro::this_coro::io_executor;
      iocoro::steady_timer timer{ex};
      timer.expires_after(1ms);

      // Close on the same executor thread to exercise close-vs-pending-op teardown,
      // without assuming cross-thread close() is supported.
      iocoro::co_spawn(
        ex,
        [&]() -> iocoro::awaitable<void> {
          (void)co_await timer.async_wait(iocoro::use_awaitable);
          sock.close();
          co_return;
        },
        iocoro::detached);

      std::array<std::byte, 256> buf{};
      auto rr = co_await sock.async_read_some(std::span{buf});
      EXPECT_FALSE(static_cast<bool>(rr));
      co_return iocoro::ok();
    }());

  server.join();

  ASSERT_TRUE(r);
  ASSERT_TRUE(*r);
}


#include <gtest/gtest.h>

#include <iocoro/co_sleep.hpp>
#include <iocoro/error.hpp>
#include <iocoro/io_context.hpp>
#include <iocoro/ip/tcp.hpp>
#include <iocoro/ip/udp.hpp>
#include <iocoro/iocoro.hpp>
#include <iocoro/steady_timer.hpp>
#include <iocoro/this_coro.hpp>

#include "test_util.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <span>
#include <stop_token>
#include <thread>

namespace iocoro::test {

using namespace std::chrono_literals;

TEST(this_coro_stop_token_test, yields_token_and_observes_stop_after_cache) {
  iocoro::io_context ctx;
  std::stop_source stop_src{};

  std::atomic<bool> saw_stop{false};

  auto task = [&]() -> iocoro::awaitable<void> {
    auto tok = co_await iocoro::this_coro::stop_token;
    EXPECT_TRUE(tok.stop_possible());

    // Allow the test thread to request stop.
    co_await iocoro::co_sleep(5ms);
    saw_stop.store(tok.stop_requested());
    co_return;
  };

  std::jthread stopper{[&]() {
    std::this_thread::sleep_for(1ms);
    stop_src.request_stop();
  }};

  auto r = iocoro::test::sync_wait(
    ctx, iocoro::co_spawn(ctx.get_executor(), stop_src.get_token(), task(), iocoro::use_awaitable));
  ASSERT_TRUE(r);
  EXPECT_TRUE(saw_stop.load());
}

TEST(this_coro_stop_token_test, stop_cancels_steady_timer_wait) {
  iocoro::io_context ctx;
  std::stop_source stop_src{};
  auto aborted = iocoro::make_error_code(iocoro::error::operation_aborted);

  auto task = [&]() -> iocoro::awaitable<iocoro::result<void>> {
    auto ex = co_await iocoro::this_coro::io_executor;
    iocoro::steady_timer t{ex};
    t.expires_after(24h);
    co_return co_await t.async_wait(iocoro::use_awaitable);
  };

  std::jthread stopper{[&]() {
    std::this_thread::sleep_for(1ms);
    stop_src.request_stop();
  }};

  auto r = iocoro::test::sync_wait(
    ctx, iocoro::co_spawn(ctx.get_executor(), stop_src.get_token(), task(), iocoro::use_awaitable));

  ASSERT_TRUE(r);
  ASSERT_FALSE(static_cast<bool>(*r));
  EXPECT_EQ(r->error(), aborted);
}

TEST(this_coro_stop_token_test, stop_cancels_tcp_accept_pending) {
  iocoro::io_context ctx;
  std::stop_source stop_src{};
  auto aborted = iocoro::make_error_code(iocoro::error::operation_aborted);

  iocoro::ip::tcp::acceptor acc{ctx};
  auto lr = acc.listen(iocoro::ip::tcp::endpoint{iocoro::ip::address_v4::loopback(), 0});
  ASSERT_TRUE(lr) << lr.error().message();

  auto task = [&]() -> iocoro::awaitable<iocoro::result<iocoro::ip::tcp::socket>> {
    co_return co_await acc.async_accept();
  };

  std::jthread stopper{[&]() {
    std::this_thread::sleep_for(1ms);
    stop_src.request_stop();
  }};

  auto r = iocoro::test::sync_wait(
    ctx, iocoro::co_spawn(ctx.get_executor(), stop_src.get_token(), task(), iocoro::use_awaitable));

  ASSERT_TRUE(r);
  ASSERT_FALSE(static_cast<bool>(*r));
  EXPECT_EQ(r->error(), aborted);
}

TEST(this_coro_stop_token_test, stop_cancels_tcp_read_pending) {
  iocoro::io_context ctx;
  auto ex = ctx.get_executor();

  iocoro::test::tcp_blackhole_server server{500ms, /*client_rcvbuf=*/1024};
  ASSERT_GE(server.listen_fd.get(), 0);
  ASSERT_NE(server.port, 0);

  std::stop_source stop_src{};
  auto aborted = iocoro::make_error_code(iocoro::error::operation_aborted);

  std::atomic<bool> started_read{false};

  auto task = [&]() -> iocoro::awaitable<void> {
    iocoro::ip::tcp::socket sock{ex};
    auto cr =
      co_await sock.async_connect(iocoro::ip::tcp::endpoint{iocoro::ip::address_v4::loopback(),
                                                           server.port});
    if (!cr) {
      ADD_FAILURE() << cr.error().message();
      co_return;
    }

    auto read_task = [&]() -> iocoro::awaitable<iocoro::result<std::size_t>> {
      started_read.store(true, std::memory_order_release);
      std::array<std::byte, 1> buf{};
      co_return co_await sock.async_read_some(std::span{buf});
    };

    auto join = iocoro::co_spawn(ex, stop_src.get_token(), read_task(), iocoro::use_awaitable);
    auto rr = co_await std::move(join);

    if (rr) {
      ADD_FAILURE() << "expected operation_aborted";
      co_return;
    }
    EXPECT_EQ(rr.error(), aborted);
    co_return;
  };

  std::jthread stopper{[&](std::stop_token) {
    (void)iocoro::test::spin_wait_for(
      [&] { return started_read.load(std::memory_order_acquire); }, 1s);
    std::this_thread::sleep_for(1ms);
    stop_src.request_stop();
  }};

  auto r = iocoro::test::sync_wait(ctx, task());
  ASSERT_TRUE(r);
}

TEST(this_coro_stop_token_test, stop_cancels_udp_receive_pending) {
  iocoro::io_context ctx;
  std::stop_source stop_src{};
  auto aborted = iocoro::make_error_code(iocoro::error::operation_aborted);

  iocoro::ip::udp::socket sock{ctx};
  auto br = sock.bind(iocoro::ip::udp::endpoint{iocoro::ip::address_v4::loopback(), 0});
  ASSERT_TRUE(br) << br.error().message();

  std::atomic<bool> started{false};
  auto recv_task = [&]() -> iocoro::awaitable<iocoro::result<std::size_t>> {
    started.store(true, std::memory_order_release);
    std::array<std::byte, 1> buf{};
    iocoro::ip::udp::endpoint src{};
    co_return co_await sock.async_receive_from(std::span{buf}, src);
  };

  std::jthread stopper{[&](std::stop_token) {
    (void)iocoro::test::spin_wait_for([&] { return started.load(std::memory_order_acquire); }, 1s);
    std::this_thread::sleep_for(1ms);
    stop_src.request_stop();
  }};

  auto r = iocoro::test::sync_wait(
    ctx,
    iocoro::co_spawn(ctx.get_executor(), stop_src.get_token(), recv_task(), iocoro::use_awaitable));

  ASSERT_TRUE(r);
  ASSERT_FALSE(static_cast<bool>(*r));
  EXPECT_EQ(r->error(), aborted);
}

}  // namespace iocoro::test

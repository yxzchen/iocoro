#include <gtest/gtest.h>

#include <iocoro/cancellation_token.hpp>
#include <iocoro/co_sleep.hpp>
#include <iocoro/io_context.hpp>
#include <iocoro/local/endpoint.hpp>
#include <iocoro/local/stream.hpp>
#include <iocoro/steady_timer.hpp>

#include "test_util.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

#include <unistd.h>

namespace {

using namespace std::chrono_literals;

struct unlink_guard {
  std::string path{};
  ~unlink_guard() {
    if (!path.empty()) {
      (void)::unlink(path.c_str());
    }
  }
};

static auto make_temp_unix_path() -> std::string {
  static std::atomic<unsigned> counter{0};
  auto const pid = static_cast<unsigned long>(::getpid());
  auto const n = counter.fetch_add(1, std::memory_order_relaxed);
  return std::string("/tmp/iocoro_cancellation_test_") + std::to_string(pid) + "_" +
         std::to_string(n) + ".sock";
}

TEST(cancellation_token_test, registration_reset_prevents_invocation) {
  iocoro::cancellation_source src{};
  auto tok = src.token();

  std::atomic<int> called{0};
  {
    auto reg = tok.register_callback([&called] { called.fetch_add(1, std::memory_order_relaxed); });
    (void)reg;
  }

  src.request_cancel();
  EXPECT_EQ(called.load(std::memory_order_relaxed), 0);
}

TEST(cancellation_token_test, register_after_cancel_invokes_immediately) {
  iocoro::cancellation_source src{};
  auto tok = src.token();

  src.request_cancel();

  std::atomic<int> called{0};
  auto reg = tok.register_callback([&called] { called.fetch_add(1, std::memory_order_relaxed); });
  (void)reg;

  EXPECT_EQ(called.load(std::memory_order_relaxed), 1);
}

TEST(steady_timer_test, cancellation_token_does_not_hang_under_race) {
  // This is a stress test for the "cancel between token callback and timer arming" window.
  // It should never hang and should usually observe operation_aborted.
  iocoro::io_context ctx;
  auto ex = ctx.get_executor();

  for (int i = 0; i < 200; ++i) {
    auto ec = iocoro::sync_wait_for(ctx, 1s, [&]() -> iocoro::awaitable<std::error_code> {
      iocoro::steady_timer t{ex};
      t.expires_after(5s);

      iocoro::cancellation_source src{};
      auto tok = src.token();

      std::atomic<bool> go{false};
      std::thread th([&] {
        while (!go.load(std::memory_order_acquire)) {
        }
        src.request_cancel();
      });

      go.store(true, std::memory_order_release);
      auto out = co_await t.async_wait(iocoro::use_awaitable, tok);

      th.join();
      co_return out;
    }());

    EXPECT_EQ(ec, iocoro::error::operation_aborted);
  }
}

TEST(local_stream_socket_test, read_some_with_cancellation_token_aborts) {
  iocoro::io_context ctx;
  auto ex = ctx.get_executor();

  auto path = make_temp_unix_path();
  unlink_guard g{path};

  auto ep_r = iocoro::local::endpoint::from_path(path);
  ASSERT_TRUE(ep_r) << ep_r.error().message();
  auto ep = *ep_r;

  auto got = iocoro::sync_wait_for(ctx, 1s, [&]() -> iocoro::awaitable<std::error_code> {
    iocoro::local::stream::acceptor a{ex};
    if (auto ec = a.listen(ep, 16)) {
      co_return ec;
    }

    iocoro::cancellation_source src{};
    std::error_code read_ec{};

    auto server_task = iocoro::co_spawn(
      ex,
      [&]() -> iocoro::awaitable<void> {
        auto accepted = co_await a.async_accept();
        if (!accepted) {
          read_ec = accepted.error();
          co_return;
        }

        auto s = std::move(*accepted);
        std::array<std::byte, 8> buf{};
        auto r = co_await s.async_read_some(buf, src.token());
        if (!r) {
          read_ec = r.error();
        } else {
          read_ec = {};
        }
      },
      iocoro::use_awaitable);

    // Client connects but does not send anything, so server read blocks.
    iocoro::local::stream::socket c{ex};
    if (auto ec = co_await c.async_connect(ep)) {
      co_return ec;
    }

    (void)co_await iocoro::co_sleep(ex, 10ms);
    src.request_cancel();

    try {
      co_await std::move(server_task);
    } catch (...) {
    }

    co_return read_ec;
  }());

  EXPECT_EQ(got, iocoro::error::operation_aborted);
}

}  // namespace



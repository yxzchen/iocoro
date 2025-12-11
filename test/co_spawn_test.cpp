#include <xz/io.hpp>
#include <xz/io/src.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>

using namespace std::chrono_literals;
using namespace xz::io;

TEST(CoSpawnTest, BasicDetached) {
  io_context ctx;
  std::atomic<int> counter{0};

  auto simple_task = [&]() -> task<void> {
    counter++;
    co_return;
  };

  co_spawn(ctx, simple_task(), use_detached);

  ctx.run();
  EXPECT_EQ(counter.load(), 1);
}

TEST(CoSpawnTest, MultipleSpawns) {
  io_context ctx;
  std::atomic<int> counter{0};

  auto increment_task = [&]() -> task<void> {
    counter++;
    co_return;
  };

  co_spawn(ctx, increment_task(), use_detached);
  co_spawn(ctx, increment_task(), use_detached);
  co_spawn(ctx, increment_task(), use_detached);

  ctx.run();
  EXPECT_EQ(counter.load(), 3);
}

TEST(CoSpawnTest, WithCallable) {
  io_context ctx;
  std::atomic<bool> executed{false};

  // Pass a callable directly (lambda that returns a task)
  co_spawn(ctx, [&]() -> task<void> {
    executed.store(true);
    co_return;
  }, use_detached);

  ctx.run();
  EXPECT_TRUE(executed.load());
}

TEST(CoSpawnTest, WithTimer) {
  io_context ctx;
  std::atomic<bool> timer_fired{false};

  auto timer_task = [&]() -> task<void> {
    // Create a simple async operation using a timer
    ctx.schedule_timer(50ms, [&]() {
      timer_fired.store(true);
      ctx.stop();
    });
    co_return;
  };

  co_spawn(ctx, timer_task(), use_detached);

  auto start = std::chrono::steady_clock::now();
  ctx.run();
  auto elapsed = std::chrono::steady_clock::now() - start;

  EXPECT_TRUE(timer_fired.load());
  EXPECT_GE(elapsed, 49ms);
  EXPECT_LT(elapsed, 150ms);
}

TEST(CoSpawnTest, ExceptionHandling) {
  io_context ctx;
  std::atomic<int> counter{0};

  auto throwing_task = [&]() -> task<void> {
    counter++;
    throw std::runtime_error("test exception");
    co_return;
  };

  auto normal_task = [&]() -> task<void> {
    counter++;
    co_return;
  };

  // Spawn a task that throws
  co_spawn(ctx, throwing_task(), use_detached);
  // Spawn a normal task to ensure execution continues
  co_spawn(ctx, normal_task(), use_detached);

  // Should not throw - exceptions are swallowed in detached mode
  EXPECT_NO_THROW(ctx.run());

  // Both tasks should have started (counter should be 2)
  EXPECT_EQ(counter.load(), 2);
}

TEST(CoSpawnTest, WithTcpSocket) {
  io_context ctx;
  tcp_socket socket(ctx);
  std::atomic<bool> connected{false};

  auto endpoint = ip::tcp_endpoint{ip::address_v4{{127, 0, 0, 1}}, 6379};

  co_spawn(ctx, [&]() -> task<void> {
    try {
      co_await socket.async_connect(endpoint, 1000ms);
      connected.store(true);
      socket.close();
    } catch (std::system_error const&) {
      // Connection failed
    }
  }, use_detached);

  ctx.run();
  EXPECT_TRUE(connected.load());
}

TEST(CoSpawnTest, ChainedOperations) {
  io_context ctx;
  tcp_socket socket(ctx);
  std::atomic<bool> success{false};

  auto endpoint = ip::tcp_endpoint{ip::address_v4{{127, 0, 0, 1}}, 6379};

  co_spawn(ctx, [&]() -> task<void> {
    try {
      co_await socket.async_connect(endpoint, 1000ms);

      // Send PING
      std::string cmd = "*1\r\n$4\r\nPING\r\n";
      co_await socket.async_write_some(
          std::span<char const>{cmd.data(), cmd.size()}, 1000ms);

      // Read response
      char buffer[256]{};
      auto bytes_read = co_await socket.async_read_some(
          std::span<char>{buffer, sizeof(buffer)}, 1000ms);

      if (bytes_read > 0) {
        std::string response(buffer, bytes_read);
        if (response.find("PONG") != std::string::npos) {
          success.store(true);
        }
      }

      socket.close();
    } catch (std::system_error const&) {
      // Operation failed
    }
  }, use_detached);

  ctx.run();
  EXPECT_TRUE(success.load());
}

TEST(CoSpawnTest, TaskWithReturnValue) {
  io_context ctx;
  std::atomic<int> result{0};

  auto value_task = [&]() -> task<int> {
    co_return 42;
  };

  co_spawn(ctx, [&, value_task]() -> task<void> {
    auto value = co_await value_task();
    result.store(value);
  }, use_detached);

  ctx.run();
  EXPECT_EQ(result.load(), 42);
}

TEST(CoSpawnTest, MultipleNestedTasks) {
  io_context ctx;
  std::atomic<int> sum{0};

  auto add_value = [&](int value) -> task<void> {
    sum.fetch_add(value);
    co_return;
  };

  co_spawn(ctx, [&, add_value]() -> task<void> {
    co_await add_value(10);
    co_await add_value(20);
    co_await add_value(30);
  }, use_detached);

  ctx.run();
  EXPECT_EQ(sum.load(), 60);
}

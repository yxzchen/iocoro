#include <xz/io.hpp>
#include <xz/io/src.hpp>

#include <gtest/gtest.h>

#include <chrono>

using namespace std::chrono_literals;
using namespace xz::io;

class TcpSocketTest : public ::testing::Test {
 protected:
  io_context ctx;

  // Helper to run a task - just starts it, the task integrates with event loop via async ops
  template<typename TaskFunc>
  void run_task(TaskFunc&& func) {
    auto t = func();
    t.resume();  // Start the coroutine
    ctx.run();   // Run event loop
  }
};

TEST_F(TcpSocketTest, BasicConstruction) {
  tcp_socket socket(ctx);
  EXPECT_FALSE(socket.is_open());
}

TEST_F(TcpSocketTest, ConnectToRedis) {
  tcp_socket socket(ctx);

  auto endpoint = ip::tcp_endpoint{ip::address_v4{{127, 0, 0, 1}}, 6379};

  bool connected = false;

  auto connect_task = [&]() -> task<void> {
    try {
      co_await socket.async_connect(endpoint, 1000ms);
      connected = true;
    } catch (std::system_error const&) {
      connected = false;
    }
  };

  run_task(connect_task);

  EXPECT_TRUE(connected) << "Failed to connect to Redis";
  EXPECT_TRUE(socket.is_open());

  socket.close();
  EXPECT_FALSE(socket.is_open());
}

TEST_F(TcpSocketTest, SendPingToRedis) {
  tcp_socket socket(ctx);
  auto endpoint = ip::tcp_endpoint{ip::address_v4{{127, 0, 0, 1}}, 6379};

  bool success = false;
  std::string response;

  auto test_task = [&]() -> task<void> {
    try {
      co_await socket.async_connect(endpoint, 1000ms);
    } catch (std::system_error const&) {
        co_return;
    }

    // Send PING command
    std::string cmd = "*1\r\n$4\r\nPING\r\n";
    try {
      co_await socket.async_write_some(
          std::span<char const>{cmd.data(), cmd.size()}, 1000ms);
    } catch (std::system_error const&) {
        co_return;
    }

    // Read response
    char buffer[256]{};
    try {
      auto bytes_read = co_await socket.async_read_some(
          std::span<char>{buffer, sizeof(buffer)}, 1000ms);
      if (bytes_read > 0) {
        response = std::string(buffer, bytes_read);
        success = true;
      }
    } catch (std::system_error const&) {
      // Ignore read errors for this test
    }

  };

  run_task(test_task);

  EXPECT_TRUE(success);
  EXPECT_NE(response.find("PONG"), std::string::npos);
}

TEST_F(TcpSocketTest, SetAndGetRedis) {
  tcp_socket socket(ctx);
  auto endpoint = ip::tcp_endpoint{ip::address_v4{{127, 0, 0, 1}}, 6379};

  bool success = false;
  std::string get_response;

  auto test_task = [&]() -> task<void> {
    try {
      co_await socket.async_connect(endpoint, 1000ms);
    } catch (std::system_error const&) {
        co_return;
    }

    // SET test_key "hello"
    std::string set_cmd = "*3\r\n$3\r\nSET\r\n$8\r\ntest_key\r\n$5\r\nhello\r\n";
    try {
      co_await socket.async_write_some(
          std::span<char const>{set_cmd.data(), set_cmd.size()}, 1000ms);
    } catch (std::system_error const&) {
        co_return;
    }

    // Read SET response
    char buffer1[256]{};
    try {
      co_await socket.async_read_some(
          std::span<char>{buffer1, sizeof(buffer1)}, 1000ms);
    } catch (std::system_error const&) {
        co_return;
    }

    // GET test_key
    std::string get_cmd = "*2\r\n$3\r\nGET\r\n$8\r\ntest_key\r\n";
    try {
      co_await socket.async_write_some(
          std::span<char const>{get_cmd.data(), get_cmd.size()}, 1000ms);
    } catch (std::system_error const&) {
        co_return;
    }

    // Read GET response
    char buffer2[256]{};
    try {
      auto bytes_read2 = co_await socket.async_read_some(
          std::span<char>{buffer2, sizeof(buffer2)}, 1000ms);
      if (bytes_read2 > 0) {
        get_response = std::string(buffer2, bytes_read2);
        success = true;
      }
    } catch (std::system_error const&) {
      // Error reading GET response
    }

    // Cleanup: DEL test_key
    std::string del_cmd = "*2\r\n$3\r\nDEL\r\n$8\r\ntest_key\r\n";
    try {
      co_await socket.async_write_some(
          std::span<char const>{del_cmd.data(), del_cmd.size()}, 1000ms);
    } catch (std::system_error const&) {
      // Ignore cleanup errors
    }

  };

  run_task(test_task);

  EXPECT_TRUE(success);
  EXPECT_NE(get_response.find("hello"), std::string::npos);
}

TEST_F(TcpSocketTest, Timeout) {
  tcp_socket socket(ctx);
  // Connect to non-routable address to trigger timeout
  auto endpoint = ip::tcp_endpoint{ip::address_v4::from_string("104.119.104.223"), 80};

  bool failed = false;

  auto test_task = [&]() -> task<void> {
    try {
      co_await socket.async_connect(endpoint, 3ms);
    } catch (std::system_error const&) {
      failed = true;  // Should fail (either timeout or connection refused)
    }
  };

  auto start = std::chrono::steady_clock::now();
  run_task(test_task);
  auto elapsed = std::chrono::steady_clock::now() - start;

  EXPECT_TRUE(failed);  // Should fail
  // Just verify it completed reasonably fast (not hanging)
  EXPECT_LT(elapsed, 1000ms);
}

TEST_F(TcpSocketTest, SocketOptions) {
  tcp_socket socket(ctx);
  auto endpoint = ip::tcp_endpoint{ip::address_v4{{127, 0, 0, 1}}, 6379};

  bool connected = false;

  auto test_task = [&]() -> task<void> {
    try {
      co_await socket.async_connect(endpoint, 1000ms);
      connected = true;
    } catch (std::system_error const&) {
      connected = false;
    }
  };

  run_task(test_task);

  ASSERT_TRUE(connected);

  // Test socket options
  auto ec1 = socket.set_option_nodelay(true);
  EXPECT_FALSE(ec1);

  auto ec2 = socket.set_option_keepalive(true);
  EXPECT_FALSE(ec2);
}

TEST_F(TcpSocketTest, Endpoints) {
  tcp_socket socket(ctx);
  auto endpoint = ip::tcp_endpoint{ip::address_v4{{127, 0, 0, 1}}, 6379};

  bool connected = false;

  auto test_task = [&]() -> task<void> {
    try {
      co_await socket.async_connect(endpoint, 1000ms);
      connected = true;
    } catch (std::system_error const&) {
      connected = false;
    }
  };

  run_task(test_task);

  ASSERT_TRUE(connected);

  auto local = socket.local_endpoint();
  EXPECT_TRUE(local.has_value());
  if (local) {
    EXPECT_TRUE(local->is_v4());
    EXPECT_NE(local->port(), 0);
  }

  auto remote = socket.remote_endpoint();
  EXPECT_TRUE(remote.has_value());
  if (remote) {
    EXPECT_TRUE(remote->is_v4());
    EXPECT_EQ(remote->port(), 6379);
  }
}

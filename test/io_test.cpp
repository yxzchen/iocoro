#include <xz/io/io.hpp>
#include <xz/io/src.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <thread>

using namespace xz::io;
using namespace std::chrono_literals;

// Test fixture for I/O tests
class IOTest : public ::testing::Test {
 protected:
  io_context ctx;
};

// ============================================================================
// IP Address Tests
// ============================================================================

TEST(IPTest, IPv4Basics) {
  auto addr = ip::address_v4::from_string("192.168.1.1");
  EXPECT_EQ(addr.to_string(), "192.168.1.1");

  auto loopback = ip::address_v4::loopback();
  EXPECT_EQ(loopback.to_string(), "127.0.0.1");

  auto any = ip::address_v4::any();
  EXPECT_EQ(any.to_string(), "0.0.0.0");
}

TEST(IPTest, IPv6Basics) {
  auto loopback = ip::address_v6::loopback();
  EXPECT_EQ(loopback.to_string(), "::1");

  auto any = ip::address_v6::any();
  EXPECT_EQ(any.to_string(), "::");
}

TEST(IPTest, TCPEndpoint) {
  auto addr = ip::address_v4::from_string("127.0.0.1");
  ip::tcp_endpoint ep{addr, 8080};

  EXPECT_EQ(ep.port(), 8080);
  EXPECT_FALSE(ep.is_v6());
  EXPECT_EQ(ep.to_string(), "127.0.0.1:8080");
}

// ============================================================================
// Event Loop Tests
// ============================================================================

TEST_F(IOTest, IOContextRunEmpty) {
  // Post a stop immediately
  bool stopped = false;
  ctx.post([&]() {
    stopped = true;
    ctx.stop();
  });
  ctx.run();
  EXPECT_TRUE(stopped);  // Verify the posted operation ran
}

TEST_F(IOTest, IOContextPost) {
  bool called = false;

  ctx.post([&]() {
    called = true;
    ctx.stop();
  });

  ctx.run();
  EXPECT_TRUE(called);
}

TEST_F(IOTest, IOContextStop) {
  bool stopped_called = false;
  ctx.post([&]() {
    stopped_called = true;
    ctx.stop();
  });

  ctx.run();
  EXPECT_TRUE(stopped_called);
  EXPECT_TRUE(ctx.stopped());

  // Restart should work
  ctx.restart();
  EXPECT_FALSE(ctx.stopped());
}

// ============================================================================
// TCP Socket Tests (without actual network)
// ============================================================================

TEST_F(IOTest, TCPSocketConstruction) {
  tcp_socket sock(ctx);
  EXPECT_FALSE(sock.is_open());
  EXPECT_EQ(sock.native_handle(), -1);
}

TEST_F(IOTest, TCPSocketClose) {
  tcp_socket sock(ctx);
  auto result = sock.close_nothrow();
  EXPECT_FALSE(result);  // Closing an unopened socket should not error (result == {})
}

// Note: Actual connection tests require a running server
// These would be integration tests rather than unit tests

// ============================================================================
// Error Code Tests
// ============================================================================

TEST(ErrorTest, ErrorCodeCreation) {
  auto ec = make_error_code(error::timeout);
  EXPECT_TRUE(ec);
  EXPECT_EQ(ec, make_error_code(error::timeout));
  EXPECT_NE(ec, std::error_code{});

  std::string msg = ec.message();
  EXPECT_FALSE(msg.empty());
}

TEST(ErrorTest, AllErrorCodes) {
  // Ensure all error codes can be created
  auto codes = {
      error::operation_aborted,     error::connection_refused,
      error::connection_reset,      error::timeout,
      error::eof,                   error::not_connected,
      error::already_connected,     error::address_in_use,
      error::network_unreachable,   error::host_unreachable,
      error::invalid_argument,      error::resolve_failed,
  };

  for (auto code : codes) {
    auto ec = make_error_code(code);
    EXPECT_TRUE(ec);
    EXPECT_FALSE(ec.message().empty());
  }
}

// ============================================================================
// Coroutine/Task Tests
// ============================================================================

TEST(TaskTest, SimpleTask) {
  auto simple = []() -> task<int> { co_return 42; };

  auto t = simple();
  t.resume();

  // Task completed, but we can't easily extract the value in this design
  // The value would be consumed by the awaiter
}

TEST(TaskTest, VoidTask) {
  bool executed = false;

  auto void_task = [](bool& flag) -> task<void> {
    flag = true;
    co_return;
  };

  auto t = void_task(executed);
  t.resume();

  EXPECT_TRUE(executed);
}

// ============================================================================
// Integration Test (if we had a test server)
// ============================================================================

// This test is skipped unless there's a Redis server running
TEST_F(IOTest, DISABLED_TCPConnectToRedis) {
  tcp_socket sock(ctx);
  bool connected = false;
  bool failed = false;

  auto connect_task = [](tcp_socket& s, bool& success, bool& fail) -> task<void> {
    try {
      auto ep = ip::tcp_endpoint{ip::address_v4::loopback(), 6379};
      co_await s.async_connect(ep);
      success = true;
    } catch (std::system_error const&) {
      fail = true;
    }
  };

  auto t = connect_task(sock, connected, failed);
  t.resume();

  ctx.run();

  // One of them should be true
  EXPECT_TRUE(connected || failed);
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

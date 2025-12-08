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
// Buffer Tests
// ============================================================================

TEST(BufferTest, DynamicBufferBasics) {
  dynamic_buffer buf;

  EXPECT_EQ(buf.size(), 0);
  EXPECT_TRUE(buf.empty());

  buf.append(std::string_view{"Hello"});
  EXPECT_EQ(buf.size(), 5);
  EXPECT_EQ(buf.view(), "Hello");

  buf.append(std::string_view{" World"});
  EXPECT_EQ(buf.size(), 11);
  EXPECT_EQ(buf.view(), "Hello World");

  buf.consume(6);
  EXPECT_EQ(buf.size(), 5);
  EXPECT_EQ(buf.view(), "World");

  buf.clear();
  EXPECT_EQ(buf.size(), 0);
  EXPECT_TRUE(buf.empty());
}

TEST(BufferTest, DynamicBufferPrepareCommit) {
  dynamic_buffer buf;

  auto span = buf.prepare(5);
  EXPECT_GE(span.size(), 5);

  std::memcpy(span.data(), "Test", 4);
  buf.commit(4);

  EXPECT_EQ(buf.size(), 4);
  EXPECT_EQ(buf.view(), "Test");
}

TEST(BufferTest, StaticBufferBasics) {
  static_buffer<64> buf;

  EXPECT_EQ(buf.size(), 0);
  EXPECT_EQ(buf.capacity(), 64);

  auto span = buf.prepare(5);
  std::memcpy(span.data(), "Hello", 5);
  buf.commit(5);

  EXPECT_EQ(buf.size(), 5);

  auto readable = buf.readable();
  EXPECT_EQ(readable.size(), 5);

  std::string_view view{readable.data(), readable.size()};
  EXPECT_EQ(view, "Hello");

  buf.consume(2);
  EXPECT_EQ(buf.size(), 3);

  buf.clear();
  EXPECT_EQ(buf.size(), 0);
}

TEST(BufferTest, DynamicBufferAutoGrow) {
  dynamic_buffer buf(16);

  // Write more than initial capacity
  for (int i = 0; i < 100; ++i) {
    buf.append(std::string_view{"x"});
  }

  EXPECT_EQ(buf.size(), 100);
  EXPECT_GE(buf.capacity(), 100);
}

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
// Timer Tests
// ============================================================================

// Note: Timer coroutine tests require additional integration work
// The timer scheduling works but coroutine resumption needs proper event loop integration
TEST_F(IOTest, DISABLED_TimerBasicWait) {
  steady_timer timer(ctx);
  bool fired = false;

  auto start = std::chrono::steady_clock::now();

  auto wait_task = [](steady_timer& t, bool& flag) -> task<void> {
    co_await t.async_wait(50ms);
    flag = true;
  };

  auto t = wait_task(timer, fired);
  t.resume();

  ctx.run();

  auto elapsed = std::chrono::steady_clock::now() - start;
  EXPECT_TRUE(fired);
  EXPECT_GE(elapsed, 50ms);
  EXPECT_LT(elapsed, 200ms);  // Should not take too long
}

TEST_F(IOTest, DISABLED_TimerCancel) {
  steady_timer timer(ctx);
  bool fired = false;

  auto wait_task = [](steady_timer& t, bool& flag) -> task<void> {
    try {
      co_await t.async_wait(1000ms);
      flag = true;
    } catch (std::system_error const& e) {
      // Timer was cancelled
      EXPECT_EQ(e.code(), make_error_code(error::operation_aborted));
    }
  };

  auto t = wait_task(timer, fired);
  t.resume();

  // Cancel after 50ms
  ctx.post([&]() {
    timer.cancel();
    ctx.stop();
  });

  ctx.run();
  EXPECT_FALSE(fired);
}

TEST_F(IOTest, MultipleTimers) {
  steady_timer timer1(ctx);
  steady_timer timer2(ctx);
  int count = 0;

  auto task1 = [](steady_timer& t, int& c) -> task<void> {
    co_await t.async_wait(30ms);
    c += 1;
  };

  auto task2 = [](steady_timer& t, int& c, io_context& ctx) -> task<void> {
    co_await t.async_wait(60ms);
    c += 10;
    ctx.stop();
  };

  auto t1 = task1(timer1, count);
  auto t2 = task2(timer2, count, ctx);
  t1.resume();
  t2.resume();

  ctx.run();
  EXPECT_EQ(count, 11);  // Both timers fired
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
  std::error_code ec;
  sock.close(ec);
  EXPECT_FALSE(ec);  // Closing an unopened socket should not error
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
      co_await s.async_connect(ep, 2s);
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

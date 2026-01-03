#include <gtest/gtest.h>

#include <iocoro/co_spawn.hpp>
#include <iocoro/io_context.hpp>
#include <iocoro/local.hpp>

#include "test_util.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <string>
#include <string_view>

#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>

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
  auto id = counter.fetch_add(1, std::memory_order_relaxed);
  return "/tmp/iocoro_local_dgram_test_" + std::to_string(::getpid()) + "_" +
         std::to_string(id);
}

static auto as_bytes(std::string const& s) -> std::span<std::byte const> {
  return {reinterpret_cast<std::byte const*>(s.data()), s.size()};
}

static auto as_writable_bytes(std::string& s) -> std::span<std::byte> {
  return {reinterpret_cast<std::byte*>(s.data()), s.size()};
}

// Test basic Unix domain datagram send/receive.
TEST(LocalDgramTest, BasicSendReceive) {
  iocoro::io_context ctx;

  auto receiver_path = make_temp_unix_path();
  auto sender_path = make_temp_unix_path();
  unlink_guard receiver_guard{receiver_path};
  unlink_guard sender_guard{sender_path};

  auto result = iocoro::sync_wait_for(ctx, 5s, [&]() -> iocoro::awaitable<std::string> {
    // Create receiver socket.
    iocoro::local::dgram::socket receiver{ctx};

    // Bind receiver to a path.
    auto receiver_ep_result = iocoro::local::endpoint::from_path(receiver_path);
    if (!receiver_ep_result) {
      co_return "Failed to create receiver endpoint";
    }
    auto receiver_ep = *receiver_ep_result;

    auto bind_ec = receiver.bind(receiver_ep);
    if (bind_ec) {
      co_return "Receiver bind failed: " + bind_ec.message();
    }

    // Create sender socket.
    iocoro::local::dgram::socket sender{ctx};

    // Bind sender (required for Unix domain datagram sockets to receive replies).
    auto sender_ep_result = iocoro::local::endpoint::from_path(sender_path);
    if (!sender_ep_result) {
      co_return "Failed to create sender endpoint";
    }
    auto sender_ep = *sender_ep_result;

    auto sender_bind_ec = sender.bind(sender_ep);
    if (sender_bind_ec) {
      co_return "Sender bind failed: " + sender_bind_ec.message();
    }

    // Send a message.
    std::string msg = "Hello Local Dgram!";
    auto send_result = co_await sender.async_send_to(as_bytes(msg), receiver_ep);
    if (!send_result.has_value()) {
      co_return "Send failed: " + send_result.error().message();
    }
    if (*send_result != msg.size()) {
      co_return "Send size mismatch";
    }

    // Receive the message.
    std::string recv_buf(256, '\0');
    iocoro::local::endpoint source_ep;
    auto recv_result = co_await receiver.async_receive_from(as_writable_bytes(recv_buf), source_ep);
    if (!recv_result.has_value()) {
      co_return "Receive failed: " + recv_result.error().message();
    }
    if (*recv_result != msg.size()) {
      co_return "Receive size mismatch";
    }
    recv_buf.resize(*recv_result);
    if (recv_buf != msg) {
      co_return "Message content mismatch";
    }

    // Verify source endpoint.
    // Extract the path from source_ep by casting to sockaddr_un.
    auto const* source_un = reinterpret_cast<sockaddr_un const*>(source_ep.data());
    if (std::string_view{source_un->sun_path} != sender_path) {
      co_return "Source endpoint mismatch";
    }

    co_return "OK";
  }());

  EXPECT_EQ(result, "OK");
}

// Test connected Unix domain datagram socket.
TEST(LocalDgramTest, ConnectedSocket) {
  iocoro::io_context ctx;

  auto receiver_path = make_temp_unix_path();
  auto sender_path = make_temp_unix_path();
  unlink_guard receiver_guard{receiver_path};
  unlink_guard sender_guard{sender_path};

  auto result = iocoro::sync_wait_for(ctx, 5s, [&]() -> iocoro::awaitable<std::string> {
    // Create receiver socket.
    iocoro::local::dgram::socket receiver{ctx};
    auto receiver_ep_result = iocoro::local::endpoint::from_path(receiver_path);
    if (!receiver_ep_result) {
      co_return "Failed to create receiver endpoint";
    }
    auto receiver_ep = *receiver_ep_result;

    auto bind_ec = receiver.bind(receiver_ep);
    if (bind_ec) {
      co_return "Receiver bind failed";
    }

    // Create sender socket and connect it.
    // Note: Unix domain datagram sockets require explicit binding before connect.
    iocoro::local::dgram::socket sender{ctx};
    auto sender_ep_result = iocoro::local::endpoint::from_path(sender_path);
    if (!sender_ep_result) {
      co_return "Failed to create sender endpoint";
    }
    auto sender_ep = *sender_ep_result;

    auto sender_bind_ec = sender.bind(sender_ep);
    if (sender_bind_ec) {
      co_return "Sender bind failed: " + sender_bind_ec.message();
    }

    auto connect_ec = sender.connect(receiver_ep);
    if (connect_ec) {
      co_return "Connect failed: " + connect_ec.message();
    }
    if (!sender.is_connected()) {
      co_return "Socket not connected";
    }

    // Send to the connected endpoint.
    std::string msg = "Connected Local Dgram";
    auto send_result = co_await sender.async_send_to(as_bytes(msg), receiver_ep);
    if (!send_result.has_value()) {
      co_return "Send failed: " + send_result.error().message();
    }
    if (*send_result != msg.size()) {
      co_return "Send size mismatch";
    }

    // Receive the message.
    std::string recv_buf(256, '\0');
    iocoro::local::endpoint source_ep;
    auto recv_result = co_await receiver.async_receive_from(as_writable_bytes(recv_buf), source_ep);
    if (!recv_result.has_value()) {
      co_return "Receive failed";
    }
    recv_buf.resize(*recv_result);
    if (recv_buf != msg) {
      co_return "Message mismatch";
    }

    co_return "OK";
  }());

  EXPECT_EQ(result, "OK");
}

// Test message boundary preservation.
TEST(LocalDgramTest, MessageBoundary) {
  iocoro::io_context ctx;

  auto receiver_path = make_temp_unix_path();
  auto sender_path = make_temp_unix_path();
  unlink_guard receiver_guard{receiver_path};
  unlink_guard sender_guard{sender_path};

  auto result = iocoro::sync_wait_for(ctx, 5s, [&]() -> iocoro::awaitable<std::string> {
    iocoro::local::dgram::socket receiver{ctx};
    auto receiver_ep_result = iocoro::local::endpoint::from_path(receiver_path);
    if (!receiver_ep_result) {
      co_return "Failed to create receiver endpoint";
    }
    auto receiver_ep = *receiver_ep_result;
    receiver.bind(receiver_ep);

    iocoro::local::dgram::socket sender{ctx};
    auto sender_ep_result = iocoro::local::endpoint::from_path(sender_path);
    if (!sender_ep_result) {
      co_return "Failed to create sender endpoint";
    }
    auto sender_ep = *sender_ep_result;
    sender.bind(sender_ep);

    // Send three separate messages.
    std::string msg1 = "First";
    std::string msg2 = "Second";
    std::string msg3 = "Third";

    co_await sender.async_send_to(as_bytes(msg1), receiver_ep);
    co_await sender.async_send_to(as_bytes(msg2), receiver_ep);
    co_await sender.async_send_to(as_bytes(msg3), receiver_ep);

    // Receive three separate messages (boundaries preserved).
    std::string recv_buf(256, '\0');
    iocoro::local::endpoint source_ep;

    auto result1 = co_await receiver.async_receive_from(as_writable_bytes(recv_buf), source_ep);
    if (!result1.has_value()) {
      co_return "Receive 1 failed";
    }
    recv_buf.resize(*result1);
    if (recv_buf != msg1) {
      co_return "Message 1 mismatch";
    }

    recv_buf.resize(256);
    auto result2 = co_await receiver.async_receive_from(as_writable_bytes(recv_buf), source_ep);
    if (!result2.has_value()) {
      co_return "Receive 2 failed";
    }
    recv_buf.resize(*result2);
    if (recv_buf != msg2) {
      co_return "Message 2 mismatch";
    }

    recv_buf.resize(256);
    auto result3 = co_await receiver.async_receive_from(as_writable_bytes(recv_buf), source_ep);
    if (!result3.has_value()) {
      co_return "Receive 3 failed";
    }
    recv_buf.resize(*result3);
    if (recv_buf != msg3) {
      co_return "Message 3 mismatch";
    }

    co_return "OK";
  }());

  EXPECT_EQ(result, "OK");
}

// Test that socket must be bound before receiving.
TEST(LocalDgramTest, NotBoundError) {
  iocoro::io_context ctx;

  auto result = iocoro::sync_wait_for(ctx, 5s, [&]() -> iocoro::awaitable<std::string> {
    iocoro::local::dgram::socket sock{ctx};

    // Try to receive without binding first.
    // Note: Since the socket is not opened yet (open happens in bind/connect),
    // we expect error::not_open rather than error::not_bound.
    std::string recv_buf(256, '\0');
    iocoro::local::endpoint source_ep;
    auto recv_result = co_await sock.async_receive_from(as_writable_bytes(recv_buf), source_ep);

    if (recv_result.has_value()) {
      co_return "Should have failed (not opened/bound)";
    }
    if (recv_result.error() != iocoro::error::not_open) {
      co_return "Wrong error code: expected not_open, got " + recv_result.error().message();
    }

    co_return "OK";
  }());

  EXPECT_EQ(result, "OK");
}

}  // namespace

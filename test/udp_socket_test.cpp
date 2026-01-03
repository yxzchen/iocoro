#include <gtest/gtest.h>

#include <iocoro/co_spawn.hpp>
#include <iocoro/io_context.hpp>
#include <iocoro/ip.hpp>

#include "test_util.hpp"

#include <array>
#include <cstddef>
#include <cstring>
#include <span>
#include <string>
#include <string_view>

namespace {

using namespace std::chrono_literals;

static auto as_bytes(std::string const& s) -> std::span<std::byte const> {
  return {reinterpret_cast<std::byte const*>(s.data()), s.size()};
}

static auto as_writable_bytes(std::string& s) -> std::span<std::byte> {
  return {reinterpret_cast<std::byte*>(s.data()), s.size()};
}

static auto to_string(std::span<std::byte const> data) -> std::string {
  return {reinterpret_cast<char const*>(data.data()), data.size()};
}

// Test basic UDP send/receive on loopback.
TEST(UdpSocketTest, BasicSendReceive) {
  iocoro::io_context ctx;

  auto result = iocoro::sync_wait_for(ctx, 5s, [&]() -> iocoro::awaitable<std::string> {
    // Create receiver socket.
    iocoro::ip::udp::socket receiver{ctx};

    // Bind to loopback with port 0 (auto-assign).
    auto local_ep = iocoro::ip::udp::endpoint{iocoro::ip::address_v4::loopback(), 0};
    auto bind_ec = receiver.bind(local_ep);
    if (bind_ec) {
      co_return "Bind failed: " + bind_ec.message();
    }

    // Get the assigned port.
    auto bound_ep_result = receiver.local_endpoint();
    if (!bound_ep_result.has_value()) {
      co_return "Failed to get local endpoint";
    }
    auto receiver_ep = *bound_ep_result;

    // Create sender socket.
    iocoro::ip::udp::socket sender{ctx};

    // Bind sender (optional for UDP, but helps with testing).
    auto sender_local = iocoro::ip::udp::endpoint{iocoro::ip::address_v4::loopback(), 0};
    auto sender_bind_ec = sender.bind(sender_local);
    if (sender_bind_ec) {
      co_return "Sender bind failed: " + sender_bind_ec.message();
    }

    // Send a message.
    std::string msg = "Hello UDP!";
    auto send_result = co_await sender.async_send_to(as_bytes(msg), receiver_ep);
    if (!send_result.has_value()) {
      co_return "Send failed: " + send_result.error().message();
    }
    if (*send_result != msg.size()) {
      co_return "Send size mismatch";
    }

    // Receive the message.
    std::string recv_buf(256, '\0');
    iocoro::ip::udp::endpoint source_ep;
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

    co_return "OK";
  }());

  EXPECT_EQ(result, "OK");
}

// Test connected UDP socket.
TEST(UdpSocketTest, ConnectedSocket) {
  iocoro::io_context ctx;

  auto result = iocoro::sync_wait_for(ctx, 5s, [&]() -> iocoro::awaitable<std::string> {
    // Create receiver socket.
    iocoro::ip::udp::socket receiver{ctx};
    auto local_ep = iocoro::ip::udp::endpoint{iocoro::ip::address_v4::loopback(), 0};
    auto bind_ec = receiver.bind(local_ep);
    if (bind_ec) {
      co_return "Receiver bind failed";
    }

    auto receiver_ep = *receiver.local_endpoint();

    // Create sender socket and connect it.
    iocoro::ip::udp::socket sender{ctx};
    auto connect_ec = sender.connect(receiver_ep);
    if (connect_ec) {
      co_return "Connect failed: " + connect_ec.message();
    }
    if (!sender.is_connected()) {
      co_return "Socket not connected";
    }

    // Send to the connected endpoint (should work).
    std::string msg = "Connected UDP";
    auto send_result = co_await sender.async_send_to(as_bytes(msg), receiver_ep);
    if (!send_result.has_value()) {
      co_return "Send failed: " + send_result.error().message();
    }
    if (*send_result != msg.size()) {
      co_return "Send size mismatch";
    }

    // Receive the message.
    std::string recv_buf(256, '\0');
    iocoro::ip::udp::endpoint source_ep;
    auto recv_result = co_await receiver.async_receive_from(as_writable_bytes(recv_buf), source_ep);
    if (!recv_result.has_value()) {
      co_return "Receive failed";
    }
    recv_buf.resize(*recv_result);
    if (recv_buf != msg) {
      co_return "Message mismatch";
    }

    // Try to send to a different endpoint (should fail).
    auto other_ep = iocoro::ip::udp::endpoint{iocoro::ip::address_v4::loopback(), 9999};
    auto send_other_result = co_await sender.async_send_to(as_bytes(msg), other_ep);
    if (send_other_result.has_value()) {
      co_return "Should have failed to send to different endpoint";
    }
    if (send_other_result.error() != iocoro::error::invalid_argument) {
      co_return "Wrong error code for mismatched endpoint";
    }

    co_return "OK";
  }());

  EXPECT_EQ(result, "OK");
}

// Test message boundary preservation.
TEST(UdpSocketTest, MessageBoundary) {
  iocoro::io_context ctx;

  auto result = iocoro::sync_wait_for(ctx, 5s, [&]() -> iocoro::awaitable<std::string> {
    iocoro::ip::udp::socket receiver{ctx};
    auto local_ep = iocoro::ip::udp::endpoint{iocoro::ip::address_v4::loopback(), 0};
    receiver.bind(local_ep);
    auto receiver_ep = *receiver.local_endpoint();

    iocoro::ip::udp::socket sender{ctx};
    sender.bind(iocoro::ip::udp::endpoint{iocoro::ip::address_v4::loopback(), 0});

    // Send three separate messages.
    std::string msg1 = "First";
    std::string msg2 = "Second";
    std::string msg3 = "Third";

    co_await sender.async_send_to(as_bytes(msg1), receiver_ep);
    co_await sender.async_send_to(as_bytes(msg2), receiver_ep);
    co_await sender.async_send_to(as_bytes(msg3), receiver_ep);

    // Receive three separate messages (boundaries preserved).
    std::string recv_buf(256, '\0');
    iocoro::ip::udp::endpoint source_ep;

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

// Test message truncation detection.
TEST(UdpSocketTest, MessageTruncation) {
  iocoro::io_context ctx;

  auto result = iocoro::sync_wait_for(ctx, 5s, [&]() -> iocoro::awaitable<std::string> {
    iocoro::ip::udp::socket receiver{ctx};
    auto local_ep = iocoro::ip::udp::endpoint{iocoro::ip::address_v4::loopback(), 0};
    receiver.bind(local_ep);
    auto receiver_ep = *receiver.local_endpoint();

    iocoro::ip::udp::socket sender{ctx};
    sender.bind(iocoro::ip::udp::endpoint{iocoro::ip::address_v4::loopback(), 0});

    // Send a large message.
    std::string large_msg(100, 'X');
    co_await sender.async_send_to(as_bytes(large_msg), receiver_ep);

    // Try to receive with a small buffer (should detect truncation).
    std::string small_buf(10, '\0');
    iocoro::ip::udp::endpoint source_ep;
    auto result_recv = co_await receiver.async_receive_from(as_writable_bytes(small_buf), source_ep);

    if (result_recv.has_value()) {
      co_return "Should have detected truncation";
    }
    if (result_recv.error() != iocoro::error::message_size) {
      co_return "Wrong error code for truncation";
    }

    co_return "OK";
  }());

  EXPECT_EQ(result, "OK");
}

}  // namespace

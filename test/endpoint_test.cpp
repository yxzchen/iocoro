#include <gtest/gtest.h>

#include <iocoro/error.hpp>
#include <iocoro/ip/tcp.hpp>

#include <compare>

#include <netinet/in.h>
#include <sys/socket.h>

TEST(endpoint_test, parse_ipv4_roundtrip) {
  auto ep = iocoro::ip::tcp::endpoint::from_string("127.0.0.1:8080");
  ASSERT_TRUE(ep);
  EXPECT_TRUE(ep->address().is_v4());
  EXPECT_EQ(ep->port(), 8080);
  EXPECT_EQ(ep->to_string(), "127.0.0.1:8080");
}

TEST(endpoint_test, parse_ipv6_bracketed_roundtrip) {
  auto ep = iocoro::ip::tcp::endpoint::from_string("[::1]:9090");
  ASSERT_TRUE(ep);
  EXPECT_TRUE(ep->address().is_v6());
  EXPECT_EQ(ep->port(), 9090);
  EXPECT_EQ(ep->to_string(), "[::1]:9090");
}

TEST(endpoint_test, parse_rejects_invalid_ports) {
  auto ep = iocoro::ip::tcp::endpoint::from_string("127.0.0.1:99999");
  ASSERT_FALSE(ep);
  EXPECT_EQ(ep.error(), iocoro::error::invalid_argument);
}

TEST(endpoint_test, parse_rejects_missing_port) {
  auto ep = iocoro::ip::tcp::endpoint::from_string("127.0.0.1");
  ASSERT_FALSE(ep);
  EXPECT_EQ(ep.error(), iocoro::error::invalid_argument);
}

TEST(endpoint_test, parse_rejects_unbracketed_ipv6) {
  auto ep = iocoro::ip::tcp::endpoint::from_string("::1:8080");
  ASSERT_FALSE(ep);
  EXPECT_EQ(ep.error(), iocoro::error::invalid_argument);
}

TEST(endpoint_test, ipv6_native_flowinfo_does_not_change_endpoint_equality) {
  sockaddr_in6 sa1{};
  sa1.sin6_family = AF_INET6;
  sa1.sin6_port = htons(8080);
  sa1.sin6_addr = in6addr_loopback;
  sa1.sin6_scope_id = 7;
  sa1.sin6_flowinfo = 1;

  sockaddr_in6 sa2 = sa1;
  sa2.sin6_flowinfo = 2;

  auto ep1 = iocoro::ip::tcp::endpoint::from_native(reinterpret_cast<sockaddr*>(&sa1), sizeof(sa1));
  auto ep2 = iocoro::ip::tcp::endpoint::from_native(reinterpret_cast<sockaddr*>(&sa2), sizeof(sa2));
  ASSERT_TRUE(ep1);
  ASSERT_TRUE(ep2);

  EXPECT_EQ(*ep1, *ep2);
  EXPECT_EQ((*ep1 <=> *ep2), std::strong_ordering::equal);
}

TEST(endpoint_test, address_v6_loopback_with_scope_is_not_loopback) {
  auto a = iocoro::ip::address_v6::from_string("::1%5");
  ASSERT_TRUE(a);
  EXPECT_FALSE(a->is_loopback());
}

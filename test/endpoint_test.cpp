#include <gtest/gtest.h>

#include <iocoro/error.hpp>
#include <iocoro/ip/tcp.hpp>

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

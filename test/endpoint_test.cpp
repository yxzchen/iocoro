#include <gtest/gtest.h>

#include <iocoro/impl.hpp>
#include <iocoro/ip/tcp/endpoint.hpp>

#include <system_error>

namespace {

TEST(endpoint_test, parse_ipv4_roundtrip) {
  auto ep = iocoro::ip::tcp::endpoint::from_string("127.0.0.1:8080");
  ASSERT_TRUE(ep) << ep.error().message();

  EXPECT_EQ(ep->family(), AF_INET);
  EXPECT_EQ(ep->port(), 8080);
  EXPECT_EQ(ep->address().to_string(), "127.0.0.1");
  EXPECT_EQ(ep->to_string(), "127.0.0.1:8080");

  auto ep2 = iocoro::ip::tcp::endpoint::from_string(ep->to_string());
  ASSERT_TRUE(ep2) << ep2.error().message();
  EXPECT_EQ(*ep, *ep2);
}

TEST(endpoint_test, parse_ipv6_bracketed_roundtrip) {
  auto ep = iocoro::ip::tcp::endpoint::from_string("[::1]:8080");
  ASSERT_TRUE(ep) << ep.error().message();

  EXPECT_EQ(ep->family(), AF_INET6);
  EXPECT_EQ(ep->port(), 8080);
  EXPECT_EQ(ep->address().to_string(), "::1");
  EXPECT_EQ(ep->to_string(), "[::1]:8080");

  auto ep2 = iocoro::ip::tcp::endpoint::from_string(ep->to_string());
  ASSERT_TRUE(ep2) << ep2.error().message();
  EXPECT_EQ(*ep, *ep2);
}

TEST(endpoint_test, parse_ipv6_with_scope_id) {
  auto ep = iocoro::ip::tcp::endpoint::from_string("[fe80::1%2]:80");
  ASSERT_TRUE(ep) << ep.error().message();

  EXPECT_EQ(ep->family(), AF_INET6);
  EXPECT_EQ(ep->port(), 80);
  EXPECT_EQ(ep->to_string(), "[fe80::1%2]:80");
}

TEST(endpoint_test, parse_rejects_unbracketed_ipv6) {
  auto ep = iocoro::ip::tcp::endpoint::from_string("::1:80");
  ASSERT_FALSE(ep);
  EXPECT_EQ(ep.error(), std::error_code{iocoro::error::invalid_argument});
}

TEST(endpoint_test, parse_rejects_invalid_ports) {
  {
    auto ep = iocoro::ip::tcp::endpoint::from_string("127.0.0.1:");
    ASSERT_FALSE(ep);
    EXPECT_EQ(ep.error(), std::error_code{iocoro::error::invalid_argument});
  }
  {
    auto ep = iocoro::ip::tcp::endpoint::from_string("127.0.0.1:99999");
    ASSERT_FALSE(ep);
    EXPECT_EQ(ep.error(), std::error_code{iocoro::error::invalid_argument});
  }
}

}  // namespace

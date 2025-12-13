#include <xz/io/ip.hpp>
#include <xz/io/src.hpp>

#include <gtest/gtest.h>

using namespace xz::io::ip;

TEST(IPv4Test, Construction) {
  address_v4 addr{{127, 0, 0, 1}};
  auto bytes = addr.to_bytes();
  EXPECT_EQ(bytes[0], 127);
  EXPECT_EQ(bytes[1], 0);
  EXPECT_EQ(bytes[2], 0);
  EXPECT_EQ(bytes[3], 1);
}

TEST(IPv4Test, FromString) {
  auto addr = address_v4::from_string("192.168.1.1");
  auto bytes = addr.to_bytes();
  EXPECT_EQ(bytes[0], 192);
  EXPECT_EQ(bytes[1], 168);
  EXPECT_EQ(bytes[2], 1);
  EXPECT_EQ(bytes[3], 1);
}

TEST(IPv4Test, ToString) {
  address_v4 addr{{10, 0, 0, 1}};
  EXPECT_EQ(addr.to_string(), "10.0.0.1");
}

TEST(IPv4Test, Loopback) {
  auto addr = address_v4::loopback();
  EXPECT_EQ(addr.to_string(), "127.0.0.1");
}

TEST(IPv4Test, Any) {
  auto addr = address_v4::any();
  EXPECT_EQ(addr.to_string(), "0.0.0.0");
}

TEST(IPv4Test, ToUint) {
  address_v4 addr{{192, 168, 1, 1}};
  uint32_t expected = (192u << 24) | (168u << 16) | (1u << 8) | 1u;
  EXPECT_EQ(addr.to_uint(), expected);
}

TEST(IPv4Test, Comparison) {
  address_v4 addr1{{192, 168, 1, 1}};
  address_v4 addr2{{192, 168, 1, 1}};
  address_v4 addr3{{192, 168, 1, 2}};

  EXPECT_EQ(addr1, addr2);
  EXPECT_NE(addr1, addr3);
  EXPECT_LT(addr1, addr3);
}

TEST(IPv6Test, Construction) {
  address_v6::bytes_type bytes{};
  bytes[15] = 1;  // ::1 (loopback)
  address_v6 addr{bytes};
  auto result = addr.to_bytes();
  EXPECT_EQ(result[15], 1);
}

TEST(IPv6Test, Loopback) {
  auto addr = address_v6::loopback();
  auto bytes = addr.to_bytes();
  bool all_zero_except_last = true;
  for (int i = 0; i < 15; ++i) {
    if (bytes[i] != 0) all_zero_except_last = false;
  }
  EXPECT_TRUE(all_zero_except_last);
  EXPECT_EQ(bytes[15], 1);
}

TEST(IPv6Test, Any) {
  auto addr = address_v6::any();
  auto bytes = addr.to_bytes();
  bool all_zero = true;
  for (auto byte : bytes) {
    if (byte != 0) all_zero = false;
  }
  EXPECT_TRUE(all_zero);
}

TEST(TcpEndpointTest, IPv4Endpoint) {
  address_v4 addr{{127, 0, 0, 1}};
  tcp_endpoint ep{addr, 8080};

  EXPECT_TRUE(ep.is_v4());
  EXPECT_FALSE(ep.is_v6());
  EXPECT_EQ(ep.port(), 8080);
  EXPECT_EQ(ep.get_address_v4(), addr);
}

TEST(TcpEndpointTest, IPv6Endpoint) {
  auto addr = address_v6::loopback();
  tcp_endpoint ep{addr, 9090};

  EXPECT_FALSE(ep.is_v4());
  EXPECT_TRUE(ep.is_v6());
  EXPECT_EQ(ep.port(), 9090);
}

TEST(TcpEndpointTest, SetPort) {
  address_v4 addr{{127, 0, 0, 1}};
  tcp_endpoint ep{addr, 8080};

  ep.set_port(9000);
  EXPECT_EQ(ep.port(), 9000);
}

TEST(TcpEndpointTest, ToString) {
  address_v4 addr{{192, 168, 1, 100}};
  tcp_endpoint ep{addr, 8080};

  auto str = ep.to_string();
  EXPECT_NE(str.find("192.168.1.100"), std::string::npos);
  EXPECT_NE(str.find("8080"), std::string::npos);
}

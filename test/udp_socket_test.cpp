#include <gtest/gtest.h>

#include <iocoro/io_context.hpp>
#include <iocoro/ip/udp.hpp>
#include "test_util.hpp"

#include <array>
#include <cstring>

TEST(udp_socket_test, basic_send_receive) {
  iocoro::io_context ctx;
  iocoro::ip::udp::socket s1{ctx};
  iocoro::ip::udp::socket s2{ctx};

  auto ec = s1.bind(iocoro::ip::udp::endpoint{iocoro::ip::address_v4::loopback(), 0});
  ASSERT_FALSE(ec) << ec.message();
  ec = s2.bind(iocoro::ip::udp::endpoint{iocoro::ip::address_v4::loopback(), 0});
  ASSERT_FALSE(ec) << ec.message();

  auto ep2 = s2.local_endpoint();
  ASSERT_TRUE(ep2);

  auto r = iocoro::test::sync_wait(
    ctx, [&]() -> iocoro::awaitable<iocoro::expected<std::size_t, std::error_code>> {
      std::array<std::byte, 4> out{};
      std::memcpy(out.data(), "ping", out.size());
      std::array<std::byte, 4> in{};
      iocoro::ip::udp::endpoint src{};

      auto send_r = co_await s1.async_send_to(std::span<std::byte const>{out}, *ep2);
      if (!send_r) {
        co_return iocoro::unexpected(send_r.error());
      }

      auto recv_r = co_await s2.async_receive_from(std::span{in}, src);
      if (!recv_r) {
        co_return iocoro::unexpected(recv_r.error());
      }

      co_return *recv_r;
    }());

  ASSERT_TRUE(r);
  ASSERT_TRUE(*r);
  EXPECT_EQ(**r, 4U);
}

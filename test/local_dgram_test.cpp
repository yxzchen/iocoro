#include <gtest/gtest.h>

#include <iocoro/io_context.hpp>
#include <iocoro/local/dgram.hpp>

#include "test_util.hpp"

#include <array>
#include <cstring>

TEST(local_dgram_test, send_and_receive_between_endpoints) {
  auto path1 = iocoro::test::make_temp_path("iocoro_local_dgram1");
  auto path2 = iocoro::test::make_temp_path("iocoro_local_dgram2");
  iocoro::test::unlink_path(path1);
  iocoro::test::unlink_path(path2);

  auto ep1 = iocoro::local::endpoint::from_path(path1);
  auto ep2 = iocoro::local::endpoint::from_path(path2);
  ASSERT_TRUE(ep1);
  ASSERT_TRUE(ep2);

  iocoro::io_context ctx;
  iocoro::local::dgram::socket s1{ctx};
  iocoro::local::dgram::socket s2{ctx};

  auto ec = s1.bind(*ep1);
  ASSERT_FALSE(ec) << ec.message();
  ec = s2.bind(*ep2);
  ASSERT_FALSE(ec) << ec.message();

  auto r = iocoro::test::sync_wait(
    ctx, [&]() -> iocoro::awaitable<iocoro::expected<std::size_t, std::error_code>> {
      std::array<std::byte, 4> out{};
      std::memcpy(out.data(), "ping", out.size());
      std::array<std::byte, 4> in{};
      iocoro::local::endpoint src{};

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

  iocoro::test::unlink_path(path1);
  iocoro::test::unlink_path(path2);

  ASSERT_TRUE(r);
  ASSERT_TRUE(*r);
  EXPECT_EQ(**r, 4U);
}

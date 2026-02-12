#include <gtest/gtest.h>

#include <iocoro/net/buffer.hpp>

#include <array>
#include <cstddef>

template <class T>
concept can_buffer_cast_const = requires(iocoro::net::const_buffer b) { iocoro::net::buffer_cast<T>(b); };

template <class T>
concept can_buffer_cast_mutable =
  requires(iocoro::net::mutable_buffer b) { iocoro::net::buffer_cast<T>(b); };

static_assert(can_buffer_cast_const<void const*>);
static_assert(can_buffer_cast_mutable<void*>);
static_assert(!can_buffer_cast_const<int>);
static_assert(!can_buffer_cast_mutable<int>);

TEST(buffer_test, buffer_cast_returns_underlying_pointer) {
  std::array<std::byte, 4> storage{};

  auto mutable_buf = iocoro::net::buffer(storage);
  auto* mutable_ptr = iocoro::net::buffer_cast<std::byte*>(mutable_buf);
  EXPECT_EQ(mutable_ptr, storage.data());

  iocoro::net::const_buffer const_buf{mutable_buf};
  auto const* const_ptr = iocoro::net::buffer_cast<std::byte const*>(const_buf);
  EXPECT_EQ(const_ptr, storage.data());
}

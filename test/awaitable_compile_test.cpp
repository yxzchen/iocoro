#include <gtest/gtest.h>

#include <xz/io/awaitable.hpp>
#include <xz/io/src.hpp>
#include <xz/io/this_coro.hpp>

namespace {

xz::io::awaitable<int> cold_int() { co_return 42; }

xz::io::awaitable<void> cold_void() { co_return; }

TEST(awaitable_compile_test, can_create_and_destroy_cold_coroutine_int) {
  auto a = cold_int();
  (void)a;
}

TEST(awaitable_compile_test, can_create_and_destroy_cold_coroutine_void) {
  auto a = cold_void();
  (void)a;
}

}  // namespace

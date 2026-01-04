#include <gtest/gtest.h>

#include <iocoro/cancellation_token.hpp>

#include <atomic>

namespace {

TEST(cancellation_token_test, registration_reset_prevents_invocation) {
  iocoro::cancellation_source src{};
  auto tok = src.token();

  std::atomic<int> called{0};
  {
    auto reg = tok.register_callback([&called] { called.fetch_add(1, std::memory_order_relaxed); });
    (void)reg;
  }

  src.request_cancel();
  EXPECT_EQ(called.load(std::memory_order_relaxed), 0);
}

TEST(cancellation_token_test, register_after_cancel_invokes_immediately) {
  iocoro::cancellation_source src{};
  auto tok = src.token();

  src.request_cancel();

  std::atomic<int> called{0};
  auto reg = tok.register_callback([&called] { called.fetch_add(1, std::memory_order_relaxed); });
  (void)reg;

  EXPECT_EQ(called.load(std::memory_order_relaxed), 1);
}

}  // namespace



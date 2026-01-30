#include <gtest/gtest.h>

#include <stop_token>

#include <atomic>

namespace {

TEST(stop_token_test, callback_destruction_prevents_invocation) {
  std::stop_source src{};
  auto tok = src.get_token();

  std::atomic<int> called{0};
  {
    std::stop_callback cb{tok,
                          [&called] { called.fetch_add(1, std::memory_order_relaxed); }};
    (void)cb;
  }

  src.request_stop();
  EXPECT_EQ(called.load(std::memory_order_relaxed), 0);
}

TEST(stop_token_test, callback_after_stop_invokes_immediately) {
  std::stop_source src{};
  auto tok = src.get_token();

  src.request_stop();

  std::atomic<int> called{0};
  std::stop_callback cb{tok, [&called] { called.fetch_add(1, std::memory_order_relaxed); }};
  (void)cb;

  EXPECT_EQ(called.load(std::memory_order_relaxed), 1);
}

}  // namespace



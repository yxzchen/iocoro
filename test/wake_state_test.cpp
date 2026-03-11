#include <gtest/gtest.h>

#include <iocoro/detail/wake_state.hpp>

TEST(wake_state_test, notify_only_arms_once_per_wait_window) {
  iocoro::detail::wake_state wakeup{};

  EXPECT_EQ(wakeup.current_phase(), iocoro::detail::wake_state::phase::running);
  EXPECT_FALSE(wakeup.notify());

  wakeup.begin_wait();
  EXPECT_EQ(wakeup.current_phase(), iocoro::detail::wake_state::phase::waiting);
  EXPECT_TRUE(wakeup.notify());
  EXPECT_EQ(wakeup.current_phase(), iocoro::detail::wake_state::phase::notified);
  EXPECT_FALSE(wakeup.notify());

  wakeup.consume_notification();
  EXPECT_EQ(wakeup.current_phase(), iocoro::detail::wake_state::phase::running);
}

TEST(wake_state_test, finish_wait_preserves_pending_notification) {
  iocoro::detail::wake_state wakeup{};

  wakeup.begin_wait();
  ASSERT_TRUE(wakeup.notify());

  wakeup.finish_wait();
  EXPECT_EQ(wakeup.current_phase(), iocoro::detail::wake_state::phase::notified);

  wakeup.consume_notification();
  EXPECT_EQ(wakeup.current_phase(), iocoro::detail::wake_state::phase::running);
}

TEST(wake_state_test, notify_failure_rolls_back_to_waiting_for_retry) {
  iocoro::detail::wake_state wakeup{};

  wakeup.begin_wait();
  ASSERT_TRUE(wakeup.notify());

  wakeup.notify_failed();
  EXPECT_EQ(wakeup.current_phase(), iocoro::detail::wake_state::phase::waiting);
  EXPECT_TRUE(wakeup.notify());
  EXPECT_EQ(wakeup.current_phase(), iocoro::detail::wake_state::phase::notified);
}

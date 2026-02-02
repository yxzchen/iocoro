#include <gtest/gtest.h>

#include <iocoro/detail/fd_registry.hpp>
#include <iocoro/detail/reactor_types.hpp>
#include <iocoro/error.hpp>

#include <atomic>
#include <cstddef>
#include <thread>

namespace {

struct count_state {
  std::atomic<int>* complete_calls{};
  std::atomic<int>* abort_calls{};

  void on_complete() noexcept {
    if (complete_calls != nullptr) {
      complete_calls->fetch_add(1, std::memory_order_relaxed);
    }
  }

  void on_abort(std::error_code) noexcept {
    if (abort_calls != nullptr) {
      abort_calls->fetch_add(1, std::memory_order_relaxed);
    }
  }
};

inline void abort_and_destroy(iocoro::detail::reactor_op_ptr op) noexcept {
  if (!op) {
    return;
  }
  op->vt->on_abort(op->block, iocoro::error::operation_aborted);
}

inline void complete_and_destroy(iocoro::detail::reactor_op_ptr op) noexcept {
  if (!op) {
    return;
  }
  op->vt->on_complete(op->block);
}

}  // namespace

TEST(stress_fd_registry, old_token_does_not_cancel_new_registration_on_same_fd) {
  iocoro::detail::fd_registry reg;

  std::atomic<int> c1{0};
  std::atomic<int> a1{0};
  std::atomic<int> c2{0};
  std::atomic<int> a2{0};

  constexpr int fd = 42;

  auto op1 = iocoro::detail::make_reactor_op<count_state>(count_state{&c1, &a1});
  auto r1 = reg.register_read(fd, std::move(op1));
  ASSERT_NE(r1.token, iocoro::detail::fd_registry::invalid_token);

  auto cancelled = reg.cancel(fd, iocoro::detail::fd_event_kind::read, r1.token);
  ASSERT_TRUE(cancelled.matched);
  abort_and_destroy(std::move(cancelled.removed));
  EXPECT_EQ(a1.load(std::memory_order_relaxed), 1);

  auto op2 = iocoro::detail::make_reactor_op<count_state>(count_state{&c2, &a2});
  auto r2 = reg.register_read(fd, std::move(op2));
  ASSERT_NE(r2.token, iocoro::detail::fd_registry::invalid_token);
  ASSERT_NE(r2.token, r1.token);

  // Attempt to cancel using the stale token.
  auto stale = reg.cancel(fd, iocoro::detail::fd_event_kind::read, r1.token);
  EXPECT_FALSE(stale.matched);
  EXPECT_EQ(a2.load(std::memory_order_relaxed), 0);
  EXPECT_EQ(c2.load(std::memory_order_relaxed), 0);

  // Mark fd readable; should complete op2 exactly once.
  auto ready = reg.take_ready(fd, /*can_read=*/true, /*can_write=*/false);
  ASSERT_TRUE(static_cast<bool>(ready.read));
  complete_and_destroy(std::move(ready.read));
  EXPECT_EQ(c2.load(std::memory_order_relaxed), 1);
  EXPECT_EQ(a2.load(std::memory_order_relaxed), 0);
}

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

TEST(stress_fd_registry, concurrent_register_cancel_take_ready_does_not_crash) {
  iocoro::detail::fd_registry reg;
  constexpr int fd = 7;
  constexpr int iters = 5000;

  std::atomic<int> completed{0};
  std::atomic<int> aborted{0};
  std::atomic<std::uint64_t> last_token{0};

  std::atomic<bool> done{false};

  std::thread producer([&] {
    for (int i = 0; i < iters; ++i) {
      auto op = iocoro::detail::make_reactor_op<count_state>(count_state{&completed, &aborted});
      auto rr = reg.register_read(fd, std::move(op));
      if (rr.token != 0) {
        last_token.store(rr.token, std::memory_order_relaxed);
      }
      // Try to cancel occasionally to exercise token matching.
      if ((i % 3) == 0) {
        auto tok = last_token.load(std::memory_order_relaxed);
        if (tok != 0) {
          auto cr = reg.cancel(fd, iocoro::detail::fd_event_kind::read, tok);
          if (cr.matched) {
            abort_and_destroy(std::move(cr.removed));
          }
        }
      }
    }
    done.store(true, std::memory_order_release);
  });

  std::thread consumer([&] {
    while (!done.load(std::memory_order_acquire)) {
      auto ready = reg.take_ready(fd, /*can_read=*/true, /*can_write=*/false);
      if (ready.read) {
        complete_and_destroy(std::move(ready.read));
      } else {
        std::this_thread::yield();
      }
    }
    // Drain any leftover.
    for (;;) {
      auto ready = reg.take_ready(fd, /*can_read=*/true, /*can_write=*/false);
      if (!ready.read) {
        break;
      }
      complete_and_destroy(std::move(ready.read));
    }
  });

  producer.join();
  consumer.join();

  EXPECT_GE(completed.load(std::memory_order_relaxed) + aborted.load(std::memory_order_relaxed), 1);
}


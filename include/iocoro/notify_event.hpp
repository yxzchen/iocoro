#pragma once

#include <iocoro/any_executor.hpp>
#include <iocoro/assert.hpp>
#include <iocoro/awaitable.hpp>
#include <iocoro/completion_token.hpp>
#include <iocoro/detail/unique_function.hpp>
#include <iocoro/error.hpp>
#include <iocoro/result.hpp>

#include <atomic>
#include <coroutine>
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <stop_token>
#include <system_error>
#include <utility>

namespace iocoro {

/// Auto-reset notification event (stop-aware).
///
/// Semantics:
/// - `notify_one()` wakes exactly one waiter if present; otherwise accumulates one "ticket"
///   (sticky behavior).
/// - `async_wait(use_awaitable)` consumes a ticket immediately if available; otherwise suspends
///   until notified.
/// - If a stop request happens while waiting, the waiter is resumed and returns
///   `error::operation_aborted`.
///
/// Notes:
/// - This is effectively a counting semaphore-like primitive (unbounded ticket count).
/// - Resumption is always scheduled by posting onto the awaiting coroutine's executor.
class notify_event {
 public:
  notify_event() = default;

  notify_event(notify_event const&) = delete;
  auto operator=(notify_event const&) -> notify_event& = delete;
  notify_event(notify_event&&) = delete;
  auto operator=(notify_event&&) -> notify_event& = delete;

  ~notify_event() = default;

  void notify_one() noexcept {
    std::shared_ptr<wait_state> st{};
    {
      std::scoped_lock lk{m_};
      if (!waiters_.empty()) {
        st = std::move(waiters_.front());
        waiters_.pop_front();
      } else {
        ++tickets_;
        return;
      }
    }
    complete(std::move(st), std::error_code{});
  }

  auto async_wait(use_awaitable_t) -> awaitable<result<void>> {
    co_return co_await wait_awaiter{this};
  }

 private:
  struct wait_state {
    any_executor ex{};
    // Completion outcome:
    // 0 = pending
    // 1 = success
    // 2 = operation_aborted
    std::atomic<int> outcome{0};

    // Waiter address handshake (similar to when_state_base):
    // - nullptr: no waiter published yet
    // - done_sentinel(): completion published
    // - else: coroutine address to resume
    std::atomic<void*> waiter_addr{nullptr};

    std::shared_ptr<std::stop_callback<detail::unique_function<void()>>> stop_cb{};
  };

  static auto done_sentinel() noexcept -> void* {
    // Coroutine frame addresses are at least pointer-aligned; reserve 1.
    return reinterpret_cast<void*>(static_cast<std::uintptr_t>(1));
  }

  static void complete(std::shared_ptr<wait_state> st, std::error_code ec) noexcept {
    if (!st) {
      return;
    }

    int const out = ec ? 2 : 1;
    int expected = 0;
    if (!st->outcome.compare_exchange_strong(expected, out, std::memory_order_release,
                                             std::memory_order_relaxed)) {
      // Already completed.
      return;
    }

    void* addr = st->waiter_addr.load(std::memory_order_acquire);
    for (;;) {
      if (addr == done_sentinel()) {
        return;
      }
      if (addr == nullptr) {
        if (st->waiter_addr.compare_exchange_weak(addr, done_sentinel(), std::memory_order_acq_rel,
                                                  std::memory_order_acquire)) {
          return;
        }
        continue;
      }
      if (st->waiter_addr.compare_exchange_weak(addr, done_sentinel(), std::memory_order_acq_rel,
                                                std::memory_order_acquire)) {
        break;
      }
    }

    auto h = std::coroutine_handle<>::from_address(addr);
    auto ex = st->ex;
    IOCORO_ENSURE(ex, "notify_event: empty executor");
    ex.post([h]() mutable noexcept { h.resume(); });
  }

  void remove_waiter(wait_state const* st) noexcept {
    std::scoped_lock lk{m_};
    for (auto it = waiters_.begin(); it != waiters_.end(); ++it) {
      if (it->get() == st) {
        waiters_.erase(it);
        return;
      }
    }
  }

  struct wait_awaiter {
    notify_event* ev{};
    std::shared_ptr<wait_state> st{std::make_shared<wait_state>()};

    explicit wait_awaiter(notify_event* ev_) noexcept : ev(ev_) {}

    bool await_ready() noexcept {
      std::scoped_lock lk{ev->m_};
      if (ev->tickets_ > 0) {
        --ev->tickets_;
        st->outcome.store(1, std::memory_order_release);
        st->waiter_addr.store(done_sentinel(), std::memory_order_release);
        return true;
      }
      return false;
    }

    template <class Promise>
      requires requires(Promise& p) { p.get_executor(); }
    bool await_suspend(std::coroutine_handle<Promise> h) noexcept {
      st->ex = h.promise().get_executor();
      IOCORO_ENSURE(st->ex, "notify_event: empty executor");

      std::stop_token token{};
      if constexpr (requires { h.promise().get_stop_token(); }) {
        token = h.promise().get_stop_token();
      }
      if (token.stop_requested()) {
        st->outcome.store(2, std::memory_order_release);
        st->waiter_addr.store(done_sentinel(), std::memory_order_release);
        return false;
      }

      if (token.stop_possible()) {
        auto weak = std::weak_ptr<wait_state>{st};
        auto* owner = ev;
        st->stop_cb =
          std::make_shared<std::stop_callback<detail::unique_function<void()>>>(
            token, detail::unique_function<void()>{[weak, owner]() mutable {
              auto st = weak.lock();
              if (!st) {
                return;
              }
              owner->remove_waiter(st.get());
              notify_event::complete(std::move(st), make_error_code(error::operation_aborted));
            }});
      }

      {
        std::scoped_lock lk{ev->m_};
        if (ev->tickets_ > 0) {
          --ev->tickets_;
          st->outcome.store(1, std::memory_order_release);
          st->waiter_addr.store(done_sentinel(), std::memory_order_release);
          return false;
        }
        if (st->outcome.load(std::memory_order_acquire) != 0) {
          return false;
        }
        ev->waiters_.push_back(st);
      }

      void* expected = nullptr;
      void* desired = h.address();
      IOCORO_ENSURE(desired != done_sentinel(), "notify_event: invalid coroutine address");
      if (!st->waiter_addr.compare_exchange_strong(expected, desired, std::memory_order_acq_rel,
                                                   std::memory_order_acquire)) {
        // Either already completed (sentinel) or multiple awaiters (non-null).
        if (expected == done_sentinel()) {
          ev->remove_waiter(st.get());
          return false;
        }
        IOCORO_ENSURE(false, "notify_event: multiple awaiters are not supported");
      }

      // If completion won the race before we published the waiter address, do not suspend.
      if (st->outcome.load(std::memory_order_acquire) != 0) {
        ev->remove_waiter(st.get());
        return false;
      }

      return true;
    }

    auto await_resume() noexcept -> result<void> {
      auto const out = st->outcome.load(std::memory_order_acquire);
      if (out == 2) {
        return unexpected(make_error_code(error::operation_aborted));
      }
      return ok();
    }
  };

  std::mutex m_{};
  std::deque<std::shared_ptr<wait_state>> waiters_{};
  std::size_t tickets_{0};
};

}  // namespace iocoro


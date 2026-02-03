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
/// - `notify_one()` wakes exactly one waiter if present; otherwise accumulates one "ticket".
/// - `async_wait(use_awaitable)` consumes a ticket immediately if available; otherwise suspends
///   until notified.
/// - If a stop request happens while waiting, the waiter is resumed and returns
///   `error::operation_aborted`.
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
    std::coroutine_handle<> h{};
    any_executor ex{};
    std::error_code ec{};
    std::atomic<bool> done{false};
    std::shared_ptr<std::stop_callback<detail::unique_function<void()>>> stop_cb{};
  };

  static void complete(std::shared_ptr<wait_state> st, std::error_code ec) noexcept {
    if (!st) {
      return;
    }
    if (st->done.exchange(true, std::memory_order_acq_rel)) {
      return;
    }
    st->ec = ec;

    auto h = std::exchange(st->h, std::coroutine_handle<>{});
    if (!h) {
      return;
    }
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
    bool ready{false};

    explicit wait_awaiter(notify_event* ev_) noexcept : ev(ev_) {}

    bool await_ready() noexcept {
      std::scoped_lock lk{ev->m_};
      if (ev->tickets_ > 0) {
        --ev->tickets_;
        ready = true;
        return true;
      }
      return false;
    }

    template <class Promise>
      requires requires(Promise& p) { p.get_executor(); }
    bool await_suspend(std::coroutine_handle<Promise> h) noexcept {
      st->h = h;
      st->ex = h.promise().get_executor();
      IOCORO_ENSURE(st->ex, "notify_event: empty executor");

      std::stop_token token{};
      if constexpr (requires { h.promise().get_stop_token(); }) {
        token = h.promise().get_stop_token();
      }
      if (token.stop_requested()) {
        st->ec = make_error_code(error::operation_aborted);
        ready = true;
        return false;
      }

      {
        std::scoped_lock lk{ev->m_};
        if (ev->tickets_ > 0) {
          --ev->tickets_;
          ready = true;
          return false;
        }
        ev->waiters_.push_back(st);
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

      return true;
    }

    auto await_resume() noexcept -> result<void> {
      if (ready) {
        if (st->ec) {
          return unexpected(st->ec);
        }
        return ok();
      }
      if (st->ec) {
        return unexpected(st->ec);
      }
      return ok();
    }
  };

  std::mutex m_{};
  std::deque<std::shared_ptr<wait_state>> waiters_{};
  std::size_t tickets_{0};
};

}  // namespace iocoro


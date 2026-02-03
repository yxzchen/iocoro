#pragma once

#include <iocoro/any_executor.hpp>
#include <iocoro/assert.hpp>
#include <iocoro/awaitable.hpp>
#include <iocoro/detail/unique_function.hpp>
#include <iocoro/error.hpp>
#include <iocoro/result.hpp>

#include <atomic>
#include <coroutine>
#include <list>
#include <memory>
#include <mutex>
#include <stop_token>
#include <system_error>
#include <utility>
#include <vector>

namespace iocoro {

/// A minimal coroutine-friendly condition/event primitive.
///
/// This is intentionally *not* a `std::condition_variable` clone:
/// - Non-blocking: waiting suspends the coroutine, never blocks a thread.
/// - No spurious wakeups: `async_wait()` completes only when a notification is consumed
///   or when cancelled/destroyed.
/// - Notifications are not lost: if notify happens before a wait, it is accumulated.
/// - No mutex coupling: callers check their own state after waking.
class condition_event {
 public:
  condition_event() : st_(std::make_shared<state>()) {}

  condition_event(condition_event const&) = delete;
  auto operator=(condition_event const&) -> condition_event& = delete;

  condition_event(condition_event&&) noexcept = default;
  auto operator=(condition_event&&) noexcept -> condition_event& = default;

  ~condition_event() noexcept { abort_all_waiters(); }

  /// Notify a single waiter if present; otherwise accumulate one pending notification.
  void notify() noexcept {
    auto st = st_;
    if (!st) {
      return;
    }

    state::waiter_ptr w{};
    {
      std::scoped_lock lk{st->m};
      if (st->destroyed) {
        return;
      }
      if (!st->waiters.empty()) {
        w = std::move(st->waiters.front());
        st->waiters.pop_front();
        w->linked = false;
      } else {
        ++st->pending;
        return;
      }
    }

    state::complete(std::move(w), std::error_code{});
  }

  /// Await one notification.
  ///
  /// Returns:
  /// - `ok()` if a notification is consumed
  /// - `operation_aborted` if cancelled via stop_token or if the event is destroyed
  auto async_wait() -> awaitable<result<void>> {
    auto st = st_;
    if (!st) {
      co_return unexpected(make_error_code(error::operation_aborted));
    }
    co_return co_await wait_awaiter{std::move(st)};
  }

 private:
  struct state : std::enable_shared_from_this<state> {
    struct waiter_state;
    using waiter_ptr = std::shared_ptr<waiter_state>;
    using waiter_list = std::list<waiter_ptr>;

    struct waiter_state {
      std::coroutine_handle<> h{};
      any_executor ex{};
      std::atomic<bool> done{false};
      std::error_code ec{};
      std::unique_ptr<std::stop_callback<detail::unique_function<void()>>> stop_cb{};
      waiter_list::iterator it{};
      bool linked{false};
    };

    std::mutex m{};
    std::size_t pending{0};
    waiter_list waiters{};
    bool destroyed{false};

    static void complete(waiter_ptr w, std::error_code ec) noexcept {
      if (!w) {
        return;
      }
      if (w->done.exchange(true, std::memory_order_acq_rel)) {
        return;
      }

      w->ec = ec;
      w->stop_cb.reset();

      auto h = std::exchange(w->h, std::coroutine_handle<>{});
      if (!h) {
        return;
      }

      auto ex = w->ex;
      IOCORO_ENSURE(ex, "condition_event: empty executor in completion");
      ex.post([h]() mutable noexcept { h.resume(); });
    }
  };

  struct wait_awaiter {
    explicit wait_awaiter(std::shared_ptr<state> st_) : st(std::move(st_)) {}

    std::shared_ptr<state> st{};
    state::waiter_ptr w{};
    result<void> ready_res{};
    bool suspended{false};

    bool await_ready() const noexcept { return false; }

    template <class Promise>
      requires requires(Promise& p) { p.get_executor(); }
    bool await_suspend(std::coroutine_handle<Promise> h) {
      IOCORO_ENSURE(st, "condition_event::async_wait: empty state");

      auto ex = h.promise().get_executor();
      IOCORO_ENSURE(ex, "condition_event::async_wait: requires a bound executor");

      std::stop_token token{};
      if constexpr (requires { h.promise().get_stop_token(); }) {
        token = h.promise().get_stop_token();
        if (token.stop_possible() && token.stop_requested()) {
          suspended = false;
          ready_res = unexpected(make_error_code(error::operation_aborted));
          return false;
        }
      }

      {
        std::scoped_lock lk{st->m};
        if (st->destroyed) {
          suspended = false;
          ready_res = unexpected(make_error_code(error::operation_aborted));
          return false;
        }
        if (st->pending > 0) {
          --st->pending;
          suspended = false;
          ready_res = ok();
          return false;
        }

        w = std::make_shared<state::waiter_state>();
        w->h = h;
        w->ex = ex;
        st->waiters.push_back(w);
        w->it = std::prev(st->waiters.end());
        w->linked = true;
      }

      if (token.stop_possible()) {
        auto weak_st = std::weak_ptr<state>{st};
        auto wp = w;
        w->stop_cb = std::make_unique<std::stop_callback<detail::unique_function<void()>>>(
          token, detail::unique_function<void()>{[weak_st, wp]() mutable noexcept {
            auto st = weak_st.lock();
            if (!st) {
              return;
            }

            {
              std::scoped_lock lk{st->m};
              if (wp->linked) {
                st->waiters.erase(wp->it);
                wp->linked = false;
              }
            }

            state::complete(std::move(wp), make_error_code(error::operation_aborted));
          }});
      }

      suspended = true;
      return true;
    }

    auto await_resume() noexcept -> result<void> {
      if (!suspended) {
        return ready_res;
      }
      if (w && w->ec) {
        return unexpected(w->ec);
      }
      return ok();
    }
  };

  void abort_all_waiters() noexcept {
    auto st = std::move(st_);
    if (!st) {
      return;
    }

    std::vector<state::waiter_ptr> waiters;
    {
      std::scoped_lock lk{st->m};
      st->destroyed = true;
      waiters.reserve(st->waiters.size());
      for (auto& w : st->waiters) {
        w->linked = false;
        waiters.push_back(std::move(w));
      }
      st->waiters.clear();
    }

    for (auto& w : waiters) {
      state::complete(std::move(w), make_error_code(error::operation_aborted));
    }
  }

  std::shared_ptr<state> st_{};
};

}  // namespace iocoro


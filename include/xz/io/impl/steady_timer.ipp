#include <xz/io/assert.hpp>
#include <xz/io/detail/executor_guard.hpp>
#include <xz/io/error.hpp>
#include <xz/io/steady_timer.hpp>

#include <chrono>
#include <coroutine>
#include <functional>
#include <system_error>
#include <utility>

namespace xz::io {

inline steady_timer::steady_timer(executor ex) noexcept : ex_(ex), expiry_(clock::now()) {}

inline steady_timer::steady_timer(executor ex, time_point at) noexcept : ex_(ex), expiry_(at) {}

inline steady_timer::steady_timer(executor ex, duration after) noexcept
    : ex_(ex), expiry_(clock::now() + after) {}

inline steady_timer::~steady_timer() { (void)cancel(); }

inline auto steady_timer::expires_at(time_point at) noexcept -> std::size_t {
  expiry_ = at;
  return cancel();
}

inline auto steady_timer::expires_after(duration d) noexcept -> std::size_t {
  expiry_ = clock::now() + d;
  return cancel();
}

inline void steady_timer::reschedule() {
  if (!ex_ || ex_.stopped()) {
    th_ = {};
    return;
  }

  using namespace std::chrono;
  auto const now = clock::now();
  auto d = (expiry_ <= now) ? duration::zero() : (expiry_ - now);
  auto ms = ceil<milliseconds>(d);
  if (ms.count() < 0) {
    ms = milliseconds{0};
  }

  // Schedule a timer entry owned by this executor/context. We use an empty callback:
  // completion is observed via timer_entry waiter notification.
  th_ = ex_.schedule_timer(ms, [] {});
}

inline void steady_timer::async_wait(std::function<void(std::error_code)> h) {
  XZ_ENSURE(ex_, "steady_timer::async_wait: requires a bound executor");

  // If the executor is stopped, complete synchronously to avoid hanging.
  if (ex_.stopped()) {
    if (h) {
      h(error::operation_aborted);
    }
    return;
  }

  // (Re)start the underlying scheduled timer if needed.
  if (!th_.pending()) {
    reschedule();
  }

  if (!th_) {
    if (h) {
      h(error::operation_aborted);
    }
    return;
  }

  auto th = th_;
  th.add_waiter([ex = ex_, th, h = std::move(h)]() mutable {
    auto ec = std::error_code{};
    if (th.cancelled()) {
      ec = error::operation_aborted;
    }
    detail::executor_guard g{ex};
    if (h) {
      h(ec);
    }
  });
}

inline auto steady_timer::async_wait(use_awaitable_t) -> awaitable<std::error_code> {
  XZ_ENSURE(ex_, "steady_timer::async_wait: requires a bound executor");

  // If the executor is stopped, complete synchronously to avoid hanging.
  if (ex_.stopped()) {
    co_return error::operation_aborted;
  }

  if (!th_.pending()) {
    reschedule();
  }

  if (!th_) {
    co_return error::operation_aborted;
  }

  struct state final {
    executor ex{};
    std::coroutine_handle<> h{};
    std::error_code ec{};
  };

  struct awaiter final {
    executor ex{};
    timer_handle th{};
    std::shared_ptr<state> st{};

    bool await_ready() const noexcept {
      if (!th) {
        return true;
      }
      // If already completed/cancelled, don't suspend.
      return !th.pending();
    }

    auto await_suspend(std::coroutine_handle<> h) -> bool {
      if (!th || !th.pending()) {
        return false;
      }

      st = std::make_shared<state>();
      st->ex = ex;
      st->h = h;

      std::weak_ptr<state> w = st;
      auto th_copy = th;
      th.add_waiter([w, th_copy]() mutable {
        if (auto s = w.lock()) {
          s->ec = th_copy.cancelled() ? error::operation_aborted : std::error_code{};

          s->ex.post([s]() mutable {
            detail::executor_guard g{s->ex};
            s->h.resume();
          });
        }
      });

      return true;
    }

    auto await_resume() noexcept -> std::error_code {
      if (!th || th.cancelled()) {
        return error::operation_aborted;
      }
      if (st) {
        return st->ec;
      }
      return std::error_code{};
    }
  };

  co_return co_await awaiter{ex_, th_};
}

inline auto steady_timer::cancel() noexcept -> std::size_t {
  if (!th_) {
    return 0;
  }
  return th_.cancel();
}

}  // namespace xz::io

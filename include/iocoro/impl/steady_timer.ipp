#include <iocoro/assert.hpp>
#include <iocoro/detail/executor_guard.hpp>
#include <iocoro/detail/io_context_impl.hpp>
#include <iocoro/error.hpp>
#include <iocoro/executor.hpp>
#include <iocoro/steady_timer.hpp>

#include <chrono>
#include <coroutine>
#include <functional>
#include <system_error>
#include <utility>

namespace iocoro {

inline steady_timer::steady_timer(io_executor ex) noexcept : ex_(ex), expiry_(clock::now()) {}

inline steady_timer::steady_timer(io_executor ex, time_point at) noexcept : ex_(ex), expiry_(at) {}

inline steady_timer::steady_timer(io_executor ex, duration after) noexcept
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

  // Schedule a timer entry owned by this io_executor/context. We use an empty callback:
  // completion is observed via timer_entry waiter notification.
  auto entry = ex_.ensure_impl().schedule_timer(ms, [] {});
  th_ = timer_handle(std::move(entry));
}

inline auto steady_timer::async_wait(use_awaitable_t) -> awaitable<std::error_code> {
  IOCORO_ENSURE(ex_, "steady_timer::async_wait: requires a bound io_executor");

  // If the io_executor is stopped, complete synchronously to avoid hanging.
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
    std::coroutine_handle<> h{};
    any_executor ex{};
    std::error_code ec{};
  };

  struct awaiter final {
    explicit awaiter(timer_handle th_) : th(std::move(th_)) {}

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
      st->h = h;
      st->ex = detail::get_current_executor();

      std::weak_ptr<state> w = st;
      auto th_copy = th;
      th.add_waiter([w, th_copy]() mutable {
        if (auto s = w.lock()) {
          s->ec = th_copy.cancelled() ? error::operation_aborted : std::error_code{};
          s->ex.post([s]() mutable { s->h.resume(); });
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

  co_return co_await awaiter{th_};
}

inline auto steady_timer::cancel() noexcept -> std::size_t {
  if (!th_) {
    return 0;
  }
  return th_.cancel();
}

}  // namespace iocoro

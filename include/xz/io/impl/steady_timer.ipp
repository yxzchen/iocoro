#pragma once

#include <xz/io/assert.hpp>
#include <xz/io/detail/executor_guard.hpp>
#include <xz/io/detail/timer_entry.hpp>
#include <xz/io/error.hpp>
#include <xz/io/steady_timer.hpp>

#include <chrono>
#include <coroutine>
#include <functional>
#include <memory>
#include <system_error>
#include <utility>

namespace xz::io {

inline steady_timer::steady_timer(executor ex) noexcept : ex_(ex), expiry_(clock::now()) {}

inline steady_timer::steady_timer(executor ex, time_point at) noexcept : ex_(ex), expiry_(at) {}

inline steady_timer::steady_timer(executor ex, duration after) noexcept
    : ex_(ex), expiry_(clock::now() + after) {}

inline steady_timer::~steady_timer() { (void)cancel(); }

inline void steady_timer::expires_at(time_point at) noexcept {
  expiry_ = at;
  (void)cancel();
}

inline void steady_timer::expires_after(duration d) noexcept {
  expiry_ = clock::now() + d;
  (void)cancel();
}

inline void steady_timer::reschedule() noexcept {
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
      h(make_error_code(error::operation_aborted));
    }
    return;
  }

  // (Re)start the underlying scheduled timer if needed.
  if (!th_.pending()) {
    reschedule();
  }

  // Attach a completion waiter that runs via ex_.post (never inline).
  auto entry = th_.entry_;
  entry->add_waiter([ex = ex_, entry, h = std::move(h)]() mutable {
    auto ec = std::error_code{};
    if (entry->is_cancelled()) {
      ec = make_error_code(error::operation_aborted);
    }
    // Ensure handler sees correct "current executor".
    detail::executor_guard g{ex};
    if (h) {
      h(ec);
    }
  });
}

inline auto steady_timer::async_wait(use_awaitable_t) -> awaitable<void> {
  XZ_ENSURE(ex_, "steady_timer::async_wait: requires a bound executor");

  // If the executor is stopped, complete synchronously to avoid hanging.
  if (ex_.stopped()) {
    co_return;
  }

  if (!th_.pending()) {
    reschedule();
  }

  struct awaiter final {
    executor ex{};
    std::shared_ptr<detail::timer_entry> entry{};
    std::coroutine_handle<> h{};

    bool await_ready() const noexcept { return false; }

    auto await_suspend(std::coroutine_handle<> ch) noexcept -> bool {
      h = ch;
      entry->add_waiter([ex = ex, ch]() mutable {
        detail::executor_guard g{ex};
        ch.resume();
      });
      return true;
    }

    void await_resume() noexcept {}
  };

  co_await awaiter{ex_, th_.entry_};
}

inline auto steady_timer::cancel() noexcept -> std::size_t {
  if (!th_) {
    return 0;
  }
  return th_.cancel() ? 1U : 0U;
}

}  // namespace xz::io



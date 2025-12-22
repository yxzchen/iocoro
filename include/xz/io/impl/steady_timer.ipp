#pragma once

#include <xz/io/assert.hpp>
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

inline void steady_timer::expires_at(time_point at) noexcept {
  expiry_ = at;
  (void)cancel();
}

inline void steady_timer::expires_after(duration d) noexcept {
  expiry_ = clock::now() + d;
  (void)cancel();
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
      h(make_error_code(error::operation_aborted));
    }
    return;
  }

  // (Re)start the underlying scheduled timer if needed.
  if (!th_.pending()) {
    reschedule();
  }

  th_.async_wait(std::move(h));
}

inline auto steady_timer::async_wait(use_awaitable_t) -> awaitable<std::error_code> {
  XZ_ENSURE(ex_, "steady_timer::async_wait: requires a bound executor");

  // If the executor is stopped, complete synchronously to avoid hanging.
  if (ex_.stopped()) {
    co_return make_error_code(error::operation_aborted);
  }

  if (!th_.pending()) {
    reschedule();
  }

  co_return co_await th_.async_wait(use_awaitable);
}

inline auto steady_timer::cancel() noexcept -> std::size_t {
  if (!th_) {
    return 0;
  }
  return th_.cancel() ? 1U : 0U;
}

}  // namespace xz::io

#pragma once

#include <xz/io/awaitable.hpp>
#include <xz/io/detail/current_executor.hpp>
#include <xz/io/io_context.hpp>
#include <xz/io/this_coro.hpp>

#include <chrono>
#include <coroutine>
#include <memory>

namespace xz::io {

namespace detail {

struct sleep_state {
  enum class state {
    pending,
    fired,
    cancelled,
  };

  io_context* ctx = nullptr;
  detail::timer_handle handle{};
  std::coroutine_handle<> h{};
  state st = state::pending;

  void fire() {
    if (st != state::pending) return;
    st = state::fired;
    if (h) {
      detail::defer_resume(h);
    }
  }

  void cancel() {
    if (st != state::pending) return;
    st = state::cancelled;
    if (handle && ctx) {
      ctx->cancel_timer(handle);
    }
    if (h) {
      detail::defer_resume(h);
    }
  }
};

struct sleep_awaiter {
  std::shared_ptr<sleep_state> st;
  std::chrono::milliseconds duration{};

  auto await_ready() const noexcept -> bool {
    return duration.count() <= 0 || st->st == sleep_state::state::cancelled;
  }

  auto await_suspend(std::coroutine_handle<> h) -> bool {
    if (st->st == sleep_state::state::cancelled || duration.count() <= 0) {
      return false;
    }
    st->h = h;
    st->handle = st->ctx->schedule_timer(duration, [s = st]() { s->fire(); });
    return true;
  }

  void await_resume() noexcept {}
};

}  // namespace detail

/// A cancelable sleep operation. Useful when racing timeouts via when_any.
class sleep_operation {
 public:
  sleep_operation() = default;

  sleep_operation(io_context& ctx, std::chrono::milliseconds duration)
      : st_(std::make_shared<detail::sleep_state>()), duration_(duration) {
    st_->ctx = &ctx;
  }

  /// Await the sleep.
  auto wait() const -> awaitable<void> {
    if (!st_) co_return;
    co_await detail::sleep_awaiter{st_, duration_};
  }

  /// Cancel the sleep (no-op if already fired).
  void cancel() const noexcept {
    if (st_) st_->cancel();
  }

 private:
  std::shared_ptr<detail::sleep_state> st_{};
  std::chrono::milliseconds duration_{};
};

/// Sleep for a duration on a specific io_context.
inline auto co_sleep(io_context& ctx, std::chrono::milliseconds duration) -> awaitable<void> {
  sleep_operation op{ctx, duration};
  co_await op.wait();
}

/// Sleep for a duration on the current coroutine's io_context.
inline auto co_sleep(std::chrono::milliseconds duration) -> awaitable<void> {
  auto& ctx = co_await this_coro::executor;
  co_await co_sleep(ctx, duration);
}

}  // namespace xz::io



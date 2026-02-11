#pragma once

#include <iocoro/any_io_executor.hpp>
#include <iocoro/awaitable.hpp>
#include <iocoro/completion_token.hpp>
#include <iocoro/detail/io_context_impl.hpp>
#include <iocoro/detail/operation_awaiter.hpp>
#include <iocoro/error.hpp>
#include <iocoro/result.hpp>
#include <iocoro/this_coro.hpp>

#include <chrono>
#include <cstddef>
#include <memory>
#include <mutex>
#include <system_error>
#include <utility>

namespace iocoro {

/// A timer with at most one pending wait at a time.
///
/// Semantics:
/// - Each `async_wait()` creates a new timer registration in the underlying `io_context`.
/// - Starting a new wait while one is pending cancels the previous wait with
///   `error::operation_aborted`.
/// - `cancel()` cancels the current pending registration (if any).
/// - Updating expiry cancels the current pending wait (if any) and affects subsequent waits.
class steady_timer {
 public:
  using clock = std::chrono::steady_clock;
  using time_point = clock::time_point;
  using duration = clock::duration;

  explicit steady_timer(any_io_executor ex, time_point at) noexcept
      : st_(std::make_shared<shared_state>(std::move(ex), at)) {}

  explicit steady_timer(any_io_executor ex) noexcept : steady_timer(std::move(ex), clock::now()) {}

  explicit steady_timer(any_io_executor ex, duration after) noexcept
      : steady_timer(std::move(ex), clock::now() + after) {}

  steady_timer(steady_timer const&) = delete;
  auto operator=(steady_timer const&) -> steady_timer& = delete;
  steady_timer(steady_timer&&) = delete;
  auto operator=(steady_timer&&) -> steady_timer& = delete;

  ~steady_timer() noexcept {
    if (st_) {
      st_->close();
    }
  }

  auto expiry() const noexcept -> time_point {
    IOCORO_ENSURE(st_ != nullptr, "steady_timer: missing state");
    return st_->expiry();
  }

  /// Set the timer expiry time.
  void expires_at(time_point at) noexcept {
    IOCORO_ENSURE(st_ != nullptr, "steady_timer: missing state");
    st_->expires_at(at);
  }

  /// Set the timer expiry time relative to now.
  void expires_after(duration d) noexcept {
    IOCORO_ENSURE(st_ != nullptr, "steady_timer: missing state");
    st_->expires_after(d);
  }

  /// Wait until expiry as an awaitable.
  ///
  /// Returns:
  /// - `ok()` on successful timer expiry.
  auto async_wait(use_awaitable_t) -> awaitable<result<void>> {
    IOCORO_ENSURE(st_ != nullptr, "steady_timer: missing state");
    auto st = st_;
    // IMPORTANT: timer registration mutates reactor-owned state; do it on the reactor thread.
    // NOTE: If we are already on that thread, avoid an extra hop (keeps run_one()-style
    // loops deterministic w.r.t. register/cancel ordering).
    co_await this_coro::on(any_executor{st->ex});
    // Snapshot expiry *after* switching to the reactor thread. This avoids races where a foreign
    // thread updates expiry between the caller's snapshot and the actual registration, which can
    // otherwise leave a long-lived timer registered without a subsequent cancellation.
    auto const expiry_snapshot = st->expiry();
    auto r = co_await detail::operation_awaiter{
      [st, expiry_snapshot](detail::reactor_op_ptr rop) {
        return st->register_timer(expiry_snapshot, std::move(rop));
      }};
    co_return r;
  }

  /// Cancel the pending timer operation.
  void cancel() noexcept {
    if (st_) {
      st_->cancel();
    }
  }

 private:
  struct shared_state {
    explicit shared_state(any_io_executor ex_, time_point at) noexcept
        : ex(std::move(ex_)), ctx_impl(ex.io_context_ptr()), expiry_(at) {
      IOCORO_ENSURE(ex, "steady_timer: requires a non-empty IO executor");
      IOCORO_ENSURE(ctx_impl != nullptr, "steady_timer: missing io_context_impl");
    }

    auto expiry() const noexcept -> time_point {
      std::scoped_lock lk{mtx};
      return expiry_;
    }

    void expires_at(time_point at) noexcept {
      detail::io_context_impl::event_handle h{};
      {
        std::scoped_lock lk{mtx};
        expiry_ = at;
        h = std::exchange(handle, detail::io_context_impl::event_handle::invalid_handle());
      }
      if (h) {
        h.cancel();
      }
    }

    void expires_after(duration d) noexcept { expires_at(clock::now() + d); }

    void cancel() noexcept {
      auto h = take_handle();
      if (h) {
        h.cancel();
      }
    }

    void close() noexcept {
      detail::io_context_impl::event_handle h{};
      {
        std::scoped_lock lk{mtx};
        closed = true;
        h = std::exchange(handle, detail::io_context_impl::event_handle::invalid_handle());
      }
      if (h) {
        h.cancel();
      }
    }

    auto register_timer(time_point expiry_snapshot, detail::reactor_op_ptr rop) noexcept
      -> detail::io_context_impl::event_handle {
      detail::io_context_impl::event_handle old{};
      detail::io_context_impl::event_handle h{};
      {
        // SAFETY: `handle` is the only cancellation hook we have. Hold `mtx` across both
        // "cancel old handle" and "store new handle" so that `cancel()` cannot observe a
        // partially-registered operation (window between add_timer() and handle assignment).
        std::scoped_lock lk{mtx};
        if (closed) {
          rop->vt->on_abort(rop->block, error::operation_aborted);
          return detail::io_context_impl::event_handle::invalid_handle();
        }
        old = std::exchange(handle, detail::io_context_impl::event_handle::invalid_handle());
        h = ctx_impl->add_timer(expiry_snapshot, std::move(rop));
        handle = h;
      }
      if (old) {
        old.cancel();
      }
      return h;
    }

    any_io_executor ex{};
    detail::io_context_impl* ctx_impl{};

   private:
    auto take_handle() noexcept -> detail::io_context_impl::event_handle {
      std::scoped_lock lk{mtx};
      return std::exchange(handle, detail::io_context_impl::event_handle::invalid_handle());
    }

    mutable std::mutex mtx{};
    time_point expiry_{clock::now()};
    bool closed{false};
    detail::io_context_impl::event_handle handle{};
  };

  std::shared_ptr<shared_state> st_{};
};

}  // namespace iocoro

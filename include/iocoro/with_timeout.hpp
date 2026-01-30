#pragma once

#include <iocoro/any_executor.hpp>
#include <iocoro/assert.hpp>
#include <iocoro/awaitable.hpp>
#include <iocoro/cancellation_token.hpp>
#include <iocoro/completion_token.hpp>
#include <iocoro/detail/io_executor_access.hpp>
#include <iocoro/detail/executor_cast.hpp>
#include <iocoro/detail/operation_base.hpp>
#include <iocoro/error.hpp>
#include <iocoro/expected.hpp>
#include <iocoro/io_executor.hpp>
#include <iocoro/steady_timer.hpp>
#include <iocoro/this_coro.hpp>
#include <iocoro/traits/awaitable_result.hpp>
#include <iocoro/traits/timeout_result.hpp>
#include <iocoro/when_any.hpp>

#include <chrono>
#include <concepts>
#include <memory>
#include <atomic>
#include <mutex>
#include <system_error>
#include <type_traits>
#include <utility>

namespace iocoro {

namespace detail {

struct timeout_state {
  std::atomic<bool> active{true};
  std::shared_ptr<cancellation_source> src{};
  std::shared_ptr<std::atomic<bool>> fired{};

  std::mutex mtx{};
  io_context_impl::timer_event_handle handle{};
};

class timeout_timer_operation final : public operation_base {
 public:
  timeout_timer_operation(std::shared_ptr<timeout_state> st, io_context_impl* impl,
                          std::chrono::steady_clock::duration timeout) noexcept
      : st_(std::move(st)), impl_(impl), timeout_(timeout) {}

  void on_ready() noexcept override {
    if (!st_) {
      return;
    }
    if (!st_->active.exchange(false, std::memory_order_acq_rel)) {
      return;
    }
    if (st_->fired) {
      st_->fired->store(true, std::memory_order_release);
    }
    if (st_->src) {
      st_->src->request_cancel();
    }
  }

  void on_abort(std::error_code) noexcept override {
    // Ignore abort (cancel/shutdown).
  }

 private:
  void do_start(std::unique_ptr<operation_base> self) override {
    IOCORO_ENSURE(impl_, "timeout_timer_operation: empty impl");

    // schedule timer and publish handle for cancellation
    auto h = impl_->schedule_timer(timeout_, std::move(self));
    {
      std::scoped_lock lk{st_->mtx};
      st_->handle = h;
    }

    // If scope already inactive, cancel immediately.
    if (!st_->active.load(std::memory_order_acquire) && h) {
      h.cancel();
    }
  }

  std::shared_ptr<timeout_state> st_;
  io_context_impl* impl_{};
  std::chrono::steady_clock::duration timeout_{};
};

inline void cancel_timeout_timer(timeout_state& st) noexcept {
  io_context_impl::timer_event_handle h{};
  {
    std::scoped_lock lk{st.mtx};
    h = std::exchange(st.handle, io_context_impl::timer_event_handle::invalid_handle());
  }
  if (h) {
    h.cancel();
  }
}

}  // namespace detail

namespace this_coro {

class timeout_scope {
 public:
  timeout_scope() = default;

  timeout_scope(::iocoro::detail::awaitable_promise_base::cancellation_scope cancel_scope,
                std::shared_ptr<detail::timeout_state> st, cancellation_registration upstream_reg,
                std::shared_ptr<std::atomic<bool>> fired) noexcept
      : cancel_scope_(std::move(cancel_scope)),
        st_(std::move(st)),
        upstream_reg_(std::move(upstream_reg)),
        fired_(std::move(fired)) {}

  timeout_scope(timeout_scope const&) = delete;
  auto operator=(timeout_scope const&) -> timeout_scope& = delete;

  timeout_scope(timeout_scope&&) noexcept = default;
  auto operator=(timeout_scope&&) noexcept -> timeout_scope& = default;

  ~timeout_scope() { reset(); }

  auto timed_out() const noexcept -> bool {
    if (!fired_) {
      return false;
    }
    return fired_->load(std::memory_order_acquire);
  }

  void reset() noexcept {
    // 1) detach upstream cancellation
    upstream_reg_.reset();

    // 2) mark inactive
    if (st_) {
      st_->active.store(false, std::memory_order_release);
    }

    // 3) cancel timer
    if (st_) {
      detail::cancel_timeout_timer(*st_);
      st_.reset();
    }

    // 4) restore previous cancellation context
    cancel_scope_.reset();

    // 5) release fired flag
    fired_.reset();
  }

 private:
  ::iocoro::detail::awaitable_promise_base::cancellation_scope cancel_scope_{};
  std::shared_ptr<detail::timeout_state> st_{};
  cancellation_registration upstream_reg_{};
  std::shared_ptr<std::atomic<bool>> fired_{};
};

}  // namespace this_coro

namespace detail {

template <class Rep, class Period>
auto awaitable_promise_base::await_transform(this_coro::scoped_timeout_t<Rep, Period> t) noexcept {
  using duration_t = std::chrono::duration<Rep, Period>;

  struct awaiter {
    awaitable_promise_base* self;
    duration_t timeout;

    bool await_ready() noexcept { return true; }
    void await_suspend(std::coroutine_handle<>) noexcept {}

    auto await_resume() noexcept -> this_coro::timeout_scope {
      auto prev_tok = self->get_cancellation_token();

      auto ex_any = self->get_executor();
      auto ex = require_executor<io_executor>(ex_any);
      auto* impl = io_executor_access::impl(ex);
      IOCORO_ENSURE(impl, "scoped_timeout: empty io_executor impl");

      auto fired = std::make_shared<std::atomic<bool>>(false);
      auto combined_src = std::make_shared<cancellation_source>();

      cancellation_registration upstream_reg{};
      if (prev_tok) {
        upstream_reg =
          prev_tok.register_callback([combined_src]() { combined_src->request_cancel(); });
      }

      auto st = std::make_shared<timeout_state>();
      st->src = combined_src;
      st->fired = fired;

      if (timeout > duration_t::zero()) {
        auto op =
          std::make_unique<timeout_timer_operation>(st, impl, std::chrono::duration_cast<std::chrono::steady_clock::duration>(timeout));
        op->start(std::move(op));
      } else {
        fired->store(true, std::memory_order_release);
        combined_src->request_cancel();
      }

      self->set_cancellation_token(combined_src->token());

      return this_coro::timeout_scope{
        cancellation_scope{self, std::move(prev_tok)},
        std::move(st),
        std::move(upstream_reg),
        std::move(fired),
      };
    }
  };

  return awaiter{this, t.timeout};
}

}  // namespace detail

/// Detached variant (awaitable input).
///
/// Semantics:
/// - On timeout, returns timed_out without attempting to cancel `op`.
/// - `op` may continue running on ex after this returns.
template <class Awaitable>
  requires requires { typename traits::awaitable_result_t<Awaitable>; }
auto with_timeout_detached(io_executor ex, Awaitable op,
                           std::chrono::steady_clock::duration timeout)
  -> awaitable<traits::awaitable_result_t<Awaitable>> {
  using result_t = traits::awaitable_result_t<Awaitable>;
  using result_traits = traits::timeout_result_traits<result_t>;

  IOCORO_ENSURE(ex, "with_timeout_detached: requires a non-empty io_executor");

  if (timeout <= std::chrono::steady_clock::duration::zero()) {
    co_return result_traits::timed_out();
  }

  auto timer = std::make_shared<steady_timer>(ex);
  timer->expires_after(timeout);

  auto timer_wait = timer->async_wait(use_awaitable);

  // Start both concurrently; whichever finishes first determines the result.
  // NOTE: when_any does not cancel the losing task.
  auto [index, v] = co_await when_any(std::move(op), std::move(timer_wait));

  if (index == 0) {
    timer->cancel();
    co_return std::get<0>(std::move(v));
  }

  auto ec = std::get<1>(std::move(v));
  if (!ec) {
    co_return result_traits::timed_out();
  }

  // Timer wait completed due to cancellation/executor shutdown.
  // Treat it as a timer error rather than a timeout.
  co_return result_traits::from_error(ec);
}

template <class Awaitable>
  requires requires { typename traits::awaitable_result_t<Awaitable>; }
auto with_timeout_detached(Awaitable op, std::chrono::steady_clock::duration timeout)
  -> awaitable<traits::awaitable_result_t<Awaitable>> {
  auto ex_any = co_await this_coro::executor;
  auto ex = detail::require_executor<io_executor>(ex_any);
  co_return co_await with_timeout_detached(ex, std::move(op), timeout);
}

}  // namespace iocoro

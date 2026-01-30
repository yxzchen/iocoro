#pragma once

#include <iocoro/any_executor.hpp>
#include <iocoro/assert.hpp>
#include <stop_token>
#include <iocoro/detail/executor_cast.hpp>
#include <iocoro/detail/io_executor_access.hpp>
#include <iocoro/detail/operation_base.hpp>
#include <iocoro/io_executor.hpp>
#include <iocoro/this_coro.hpp>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>

namespace iocoro::detail {

/// Shared state for a scoped timeout.
struct scoped_timeout_state {
  // ---- reactor / timer ----
  std::atomic<bool> active{true};
  std::atomic<bool> fired{false};

  std::mutex mtx{};
  io_context_impl::timer_event_handle handle{};

  // ---- cancellation glue ----
  std::stop_source src{};
  std::optional<std::stop_callback<std::function<void()>>> upstream_reg{};
  awaitable_promise_base::stop_scope cancel_scope{};

  void cancel_timer() noexcept {
    io_context_impl::timer_event_handle h{};
    {
      std::scoped_lock lk{mtx};
      h = std::exchange(handle, io_context_impl::timer_event_handle::invalid_handle());
    }
    if (h) {
      h.cancel();
    }
  }

  void reset() noexcept {
    // 1) detach upstream cancellation
    upstream_reg.reset();

    // 2) mark inactive
    active.store(false, std::memory_order_release);

    // 3) cancel timer
    cancel_timer();

    // 4) restore previous cancellation context
    cancel_scope.reset();
  }

  auto timed_out() const noexcept -> bool {
    return fired.load(std::memory_order_acquire);
  }
};

struct scoped_timeout_timer_operation {
  std::weak_ptr<scoped_timeout_state> st;

  void on_complete() noexcept {
    if (auto locked = st.lock()) {
      if (locked->active.exchange(false, std::memory_order_acq_rel)) {
        locked->fired.store(true, std::memory_order_release);
        locked->src.request_stop();
      }
    }
  }

  void on_abort(std::error_code) noexcept {
    // Ignore abort (cancel/shutdown).
  }
};

inline auto scoped_timeout_get_timer_impl(awaitable_promise_base& promise,
                                          any_executor timer_ex) noexcept -> io_context_impl* {
  auto timer_any = timer_ex ? timer_ex : promise.get_executor();
  IOCORO_ENSURE(timer_any,
               "scoped_timeout: requires a timer executor (pass iocoro::io_executor explicitly)");

  auto const* io_ex_ptr = detail::any_executor_access::target<io_executor>(timer_any);
  IOCORO_ENSURE(io_ex_ptr,
               "scoped_timeout: timer executor must be iocoro::io_executor "
               "(pass it explicitly if current executor is not)");

  auto const io_ex = *io_ex_ptr;
  auto* impl = io_executor_access::impl(io_ex);
  IOCORO_ENSURE(impl, "scoped_timeout: empty io_executor impl");
  return impl;
}

}  // namespace iocoro::detail

namespace iocoro {

/// RAII handle returned by `co_await this_coro::scoped_timeout(...)`.
///
/// This is a user-visible type. It intentionally does not expose any reactor,
/// executor, or promise implementation details.
class timeout_scope {
 public:
  timeout_scope() noexcept = default;

  timeout_scope(timeout_scope const&) = delete;
  auto operator=(timeout_scope const&) -> timeout_scope& = delete;

  timeout_scope(timeout_scope&&) noexcept = default;
  auto operator=(timeout_scope&&) noexcept -> timeout_scope& = default;

  ~timeout_scope() { reset(); }

  auto timed_out() const noexcept -> bool {
    return st_ && st_->timed_out();
  }

  void reset() noexcept {
    if (auto st = std::exchange(st_, {})) {
      st->reset();
    }
  }

 private:
  std::shared_ptr<detail::scoped_timeout_state> st_{};

  explicit timeout_scope(std::shared_ptr<detail::scoped_timeout_state> st) noexcept
      : st_(std::move(st)) {}

  friend class detail::awaitable_promise_base;
};

}  // namespace iocoro

namespace iocoro::detail {

template <class Rep, class Period>
inline auto awaitable_promise_base::await_transform(this_coro::scoped_timeout_t<Rep, Period> t) noexcept {
  using duration_t = std::chrono::duration<Rep, Period>;

  struct awaiter {
    awaitable_promise_base* self{};
    any_executor timer_ex{};
    duration_t timeout_d{};

    bool await_ready() noexcept { return true; }
    void await_suspend(std::coroutine_handle<>) noexcept {}

    auto await_resume() noexcept -> timeout_scope {
      auto* impl = scoped_timeout_get_timer_impl(*self, std::move(timer_ex));

      auto prev_tok = self->get_stop_token();

      auto state = std::make_shared<scoped_timeout_state>();

      // upstream cancellation => cancel combined token (and timer)
      if (prev_tok.stop_possible()) {
        state->upstream_reg.emplace(std::move(prev_tok),
          [weak = std::weak_ptr<scoped_timeout_state>{state}]() {
            if (auto st = weak.lock()) {
              st->src.request_stop();
            }
          });
      }

      if (timeout_d > duration_t::zero()) {
        auto const steady_timeout =
          std::chrono::duration_cast<std::chrono::steady_clock::duration>(timeout_d);
        auto op =
          make_reactor_op<scoped_timeout_timer_operation>(std::weak_ptr<scoped_timeout_state>{state});
        auto handle = impl->add_timer(steady_timeout, std::move(op));
        {
          std::scoped_lock lk{state->mtx};
          state->handle = handle;
        }

        // If scope already inactive, cancel immediately.
        if (!state->active.load(std::memory_order_acquire) && handle) {
          handle.cancel();
        }
      } else {
        state->fired.store(true, std::memory_order_release);
        state->active.store(false, std::memory_order_release);
        state->src.request_stop();
      }

      self->set_stop_token(state->src.get_token());
      state->cancel_scope = awaitable_promise_base::stop_scope{self, std::move(prev_tok)};

      return timeout_scope{std::move(state)};
    }
  };

  return awaiter{this, std::move(t.timer_ex), t.timeout};
}

}  // namespace iocoro::detail

#pragma once

#include <atomic>
#include <coroutine>
#include <iocoro/any_executor.hpp>
#include <iocoro/assert.hpp>
#include <iocoro/detail/reactor_types.hpp>
#include <iocoro/detail/unique_function.hpp>
#include <iocoro/error.hpp>
#include <iocoro/result.hpp>
#include <memory>
#include <stop_token>
#include <system_error>

namespace iocoro::detail {

/// Shared state for an in-flight reactor operation.
///
/// SAFETY: completion and cancellation may race; `done` is used to guarantee exactly one
/// resumption path wins and observes the final `ec`.
struct operation_wait_state {
  std::coroutine_handle<> h{};
  any_executor ex{};
  std::error_code ec{};
  std::atomic<bool> done{false};
  std::unique_ptr<std::stop_callback<unique_function<void()>>> stop_cb{};
};

/// Awaiter that bridges a reactor operation into coroutine suspension.
///
/// Semantics:
/// - Registers a reactor operation via `register_op`.
/// - Captures the awaiting coroutine's executor and resumes by posting onto it.
/// - If a stop token is available, requests cancellation best-effort by calling `handle.cancel()`.
template <typename Factory>
struct operation_awaiter {
  Factory register_op;
  std::shared_ptr<operation_wait_state> st{std::make_shared<operation_wait_state>()};

  explicit operation_awaiter(Factory f) : register_op(std::move(f)) {}

  bool await_ready() const noexcept { return false; }

  template <class Promise>
    requires requires(Promise& p) { p.get_executor(); }
  bool await_suspend(std::coroutine_handle<Promise> h) noexcept {
    st->h = h;
    st->ex = h.promise().get_executor();
    IOCORO_ENSURE(st->ex, "operation_awaiter: empty executor");

    struct op_state {
      std::shared_ptr<operation_wait_state> st;

      explicit op_state(std::shared_ptr<operation_wait_state> s) noexcept : st(std::move(s)) {}

      void on_complete() noexcept { complete(std::error_code{}); }
      void on_abort(std::error_code ec) noexcept { complete(ec); }

      void complete(std::error_code ec) noexcept {
        if (st->done.exchange(true, std::memory_order_acq_rel)) {
          return;
        }
        st->ec = ec;

        // SAFETY: stop callback may race with completion. Resetting unregisters it and makes
        // cancellation best-effort and idempotent.
        st->stop_cb.reset();

        auto h = std::exchange(st->h, std::coroutine_handle<>{});
        if (!h) {
          return;
        }
        auto ex = st->ex;
        IOCORO_ENSURE(ex, "operation_awaiter: empty executor in completion");
        // IMPORTANT: Resume by posting onto the captured executor. This avoids resuming inline
        // on the reactor thread and keeps execution under the awaiting coroutine's executor.
        ex.post([h]() mutable noexcept { h.resume(); });
      }
    };

    auto handle = register_op(make_reactor_op<op_state>(st));

    if constexpr (requires { h.promise().get_stop_token(); }) {
      auto token = h.promise().get_stop_token();
      if (token.stop_requested()) {
        handle.cancel();
      } else {
        st->stop_cb = std::make_unique<std::stop_callback<unique_function<void()>>>(
          token, unique_function<void()>{[handle]() mutable { handle.cancel(); }});
      }
    }

    return true;
  }

  auto await_resume() noexcept -> iocoro::result<void> {
    if (st->ec) {
      return iocoro::unexpected(st->ec);
    }
    return {};
  }
};

template <typename Factory>
operation_awaiter(Factory) -> operation_awaiter<Factory>;

}  // namespace iocoro::detail

#pragma once

#include <iocoro/assert.hpp>
#include <iocoro/error.hpp>
#include <iocoro/detail/reactor_types.hpp>
#include <iocoro/detail/unique_function.hpp>
#include <iocoro/any_executor.hpp>
#include <coroutine>
#include <memory>
#include <stop_token>
#include <system_error>
#include <atomic>

namespace iocoro::detail {

/// Shared state for async operation awaiters.
/// Holds the coroutine handle, executor, and result error code.
struct operation_wait_state {
  std::coroutine_handle<> h{};
  any_executor ex{};
  std::error_code ec{};
  std::atomic<bool> done{false};
  std::unique_ptr<std::stop_callback<unique_function<void()>>> stop_cb{};
};

/// Generic awaiter for async operations.
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

        // Stop callback might race with completion; resetting unregisters it and
        // makes cancellation best-effort idempotent.
        st->stop_cb.reset();

        auto h = std::exchange(st->h, std::coroutine_handle<>{});
        if (!h) {
          return;
        }
        auto ex = st->ex;
        IOCORO_ENSURE(ex, "operation_awaiter: empty executor in completion");
        ex.post([h]() mutable noexcept { h.resume(); });
      }
    };

    auto handle = register_op(make_reactor_op<op_state>(st));

    if constexpr (requires {
                    h.promise().get_stop_token();
                  }) {
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

  auto await_resume() noexcept -> std::error_code {
    return st->ec;
  }
};

template <typename Factory>
operation_awaiter(Factory) -> operation_awaiter<Factory>;

}  // namespace iocoro::detail

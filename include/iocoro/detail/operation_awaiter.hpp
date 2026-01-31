#pragma once

#include <iocoro/assert.hpp>
#include <iocoro/error.hpp>
#include <iocoro/detail/reactor_types.hpp>
#include <iocoro/any_executor.hpp>
#include <coroutine>
#include <system_error>

namespace iocoro::detail {

/// Shared state for async operation awaiters.
/// Holds the coroutine handle, executor, and result error code.
struct operation_wait_state {
  std::coroutine_handle<> h{};
  any_executor ex{};
  std::error_code ec{};
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
  bool await_suspend(std::coroutine_handle<Promise> h) {
    st->h = h;
    st->ex = h.promise().get_executor();
    IOCORO_ENSURE(st->ex, "operation_awaiter: empty executor");

    struct op_state {
      std::shared_ptr<operation_wait_state> st;

      explicit op_state(std::shared_ptr<operation_wait_state> s) noexcept : st(std::move(s)) {}

      void on_complete() noexcept { complete(std::error_code{}); }
      void on_abort(std::error_code ec) noexcept { complete(ec); }

      void complete(std::error_code ec) noexcept {
        st->ec = ec;
        st->ex.post([st = st]() mutable { st->h.resume(); });
      }
    };

    auto handle = register_op(make_reactor_op<op_state>(st));

    // handle will be used for structured cancel. ignore for now.
    (void)handle;
    return true;
  }

  auto await_resume() noexcept -> std::error_code {
    return st->ec;
  }
};

template <typename Factory>
operation_awaiter(Factory) -> operation_awaiter<Factory>;

}  // namespace iocoro::detail

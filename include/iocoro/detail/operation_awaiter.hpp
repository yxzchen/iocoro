#pragma once

#include <stop_token>

#include <iocoro/detail/async_op.hpp>
#include <functional>
#include <optional>

namespace iocoro::detail {

/// Generic awaiter for async operations.
///
/// This awaiter eliminates code duplication across different operation types.
/// All operations follow the same pattern:
/// 1. Create shared state
/// 2. Capture current executor
/// 3. Create and start the operation
/// 4. Resume with error_code result
template <typename Factory>
struct operation_awaiter {
  Factory factory;
  std::shared_ptr<operation_wait_state> st{std::make_shared<operation_wait_state>()};
  std::optional<std::stop_callback<std::function<void()>>> reg{};

  explicit operation_awaiter(Factory f) : factory(std::move(f)) {}

  bool await_ready() const noexcept { return false; }

  template <class Promise>
    requires requires(Promise& p) { p.get_executor(); }
  bool await_suspend(std::coroutine_handle<Promise> h) {
    st->h = h;
    st->ex = h.promise().get_executor();
    IOCORO_ENSURE(st->ex, "operation_awaiter: empty executor");
    if constexpr (requires { h.promise().get_stop_token(); }) {
      auto tok = h.promise().get_stop_token();
      if (tok.stop_possible()) {
        auto op = factory(st);
        auto cancel_state = op.cancel_state_ptr();
        reg.emplace(std::move(tok), [cancel_state]() { cancel_state->cancel(); });
        op.start();
        return true;
      }
    }

    auto op = factory(st);
    op.start();
    return true;
  }

  auto await_resume() noexcept -> std::error_code {
    reg.reset();
    return st->ec;
  }
};

template <typename Factory>
operation_awaiter(Factory) -> operation_awaiter<Factory>;

}  // namespace iocoro::detail

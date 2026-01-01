#pragma once

#include <iocoro/detail/executor_guard.hpp>

#include <coroutine>
#include <memory>
#include <system_error>
#include <tuple>
#include <utility>

namespace iocoro::detail {

/// Shared state for operation awaiters.
/// Holds the coroutine handle, executor, and result error code.
struct operation_wait_state {
  std::coroutine_handle<> h{};
  any_executor ex{};
  std::error_code ec{};
};

/// Generic awaiter for operation_base-derived operations.
///
/// All operations follow the same pattern:
/// 1. Create shared state
/// 2. Capture current executor
/// 3. Create and start the operation
/// 4. Resume with error_code result
///
/// Template parameters:
/// - Operation: The operation_base-derived class to instantiate
/// - Args: Constructor arguments for the operation (excluding the beginning wait_state parameter)
///
/// Usage:
///   co_await operation_awaiter<timer_wait_operation, steady_timer*>{this};
///   co_await operation_awaiter<fd_wait_operation<Kind>, socket_impl_base*>{this};
template <typename Operation>
struct operation_awaiter {
  std::unique_ptr<Operation> op;
  std::shared_ptr<operation_wait_state> st{std::make_shared<operation_wait_state>()};

  template <typename... Args>
  explicit operation_awaiter(Args... args) {
    op = std::make_unique<Operation>(st, std::forward<Args>(args)...);
  }

  bool await_ready() const noexcept { return false; }

  bool await_suspend(std::coroutine_handle<> h) {
    st->h = h;
    st->ex = get_current_executor();

    op->start(std::move(op));
    return true;
  }

  auto await_resume() noexcept -> std::error_code { return st->ec; }
};

}  // namespace iocoro::detail

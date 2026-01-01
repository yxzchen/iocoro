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
/// - Args: Constructor arguments for the operation (excluding the final wait_state parameter)
///
/// Usage:
///   co_await operation_awaiter<timer_wait_operation, steady_timer*>{this};
///   co_await operation_awaiter<fd_wait_operation<Kind>, socket_impl_base*>{this};
template <typename Operation, typename... Args>
struct operation_awaiter {
  std::shared_ptr<operation_wait_state> st{std::make_shared<operation_wait_state>()};
  std::tuple<Args...> args;

  explicit operation_awaiter(Args... args_) noexcept
      : args(std::forward<Args>(args_)...) {}

  bool await_ready() const noexcept { return false; }

  bool await_suspend(std::coroutine_handle<> h) {
    st->h = h;
    st->ex = get_current_executor();

    // Construct operation with captured args + shared state
    auto op = std::apply(
        [this](auto&&... operation_args) {
          return std::make_unique<Operation>(std::forward<decltype(operation_args)>(operation_args)..., st);
        },
        args);
    op->start(std::move(op));
    return true;
  }

  auto await_resume() noexcept -> std::error_code { return st->ec; }
};

}  // namespace iocoro::detail

#pragma once

#include <iocoro/detail/executor_guard.hpp>
#include <iocoro/detail/operation_base.hpp>

#include <atomic>
#include <coroutine>
#include <memory>
#include <system_error>

namespace iocoro::detail {

/// Shared state for async operation awaiters.
/// Holds the coroutine handle, executor, and result error code.
struct operation_wait_state {
  std::coroutine_handle<> h{};
  any_executor ex{};
  std::error_code ec{};
};

/// Base class for async operations that provides common completion logic.
///
/// All async operations follow the same pattern:
/// 1. on_ready() / on_abort() → complete(ec)
/// 2. complete() checks atomic done flag, sets ec, posts to executor
/// 3. do_start() is operation-specific (registration logic)
///
/// Derived classes only need to implement do_start() to register with the reactor.
class async_operation : public operation_base {
 public:
  void on_ready() noexcept final { complete(std::error_code{}); }
  void on_abort(std::error_code ec) noexcept final { complete(ec); }

 protected:
  async_operation(std::shared_ptr<operation_wait_state> st) noexcept
      : st_(std::move(st)) {}

  void complete(std::error_code ec) noexcept {
    // Guard against double completion (on_ready + on_abort, or repeated signals).
    if (done.exchange(true, std::memory_order_acq_rel)) {
      return;
    }
    st_->ec = ec;

    // Resume on the caller's original executor.
    // This ensures the awaitable coroutine resumes on the same executor
    // where the user initiated the wait operation.
    st_->ex.post([st = st_]() mutable { st->h.resume(); });
  }

  std::shared_ptr<operation_wait_state> st_;
  std::atomic<bool> done{false};
};

/// Generic awaiter for async_operation-derived operations.
///
/// This awaiter eliminates code duplication across different operation types.
/// All operations follow the same pattern:
/// 1. Create shared state
/// 2. Capture current executor
/// 3. Create and start the operation
/// 4. Resume with error_code result
///
/// Template parameters:
/// - Operation: The async_operation-derived class to instantiate
///
/// Usage:
///   co_await operation_awaiter<timer_wait_operation>{this};
///   co_await operation_awaiter<fd_wait_operation<Kind>>{this};
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

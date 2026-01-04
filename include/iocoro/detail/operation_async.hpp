#pragma once

#include <iocoro/cancellation_token.hpp>
#include <iocoro/detail/executor_guard.hpp>
#include <iocoro/detail/unique_function.hpp>
#include <iocoro/detail/operation_base.hpp>
#include <iocoro/error.hpp>

#include <atomic>
#include <coroutine>
#include <memory>
#include <mutex>
#include <system_error>

namespace iocoro::detail {

/// Cancellation handshake for a single in-flight operation.
///
/// Model:
/// - The awaiter (coroutine layer) calls cancel() when the token is triggered.
/// - The reactor-facing operation publishes a cancel hook via publish().
///
/// Guarantees:
/// - If cancel() happens before publish(), the hook will be invoked immediately upon publish().
/// - If publish() happens before cancel(), the hook will be invoked immediately upon cancel().
///
/// This keeps cancellation_token out of reactor operations while still enabling race-free,
/// "pending-cancel" semantics.
struct operation_cancellation {
  std::atomic<bool> pending{false};
  std::mutex mtx{};
  unique_function<void()> hook{};

  void publish(unique_function<void()> f) noexcept {
    unique_function<void()> to_call{};
    {
      std::scoped_lock lk{mtx};
      hook = std::move(f);
      if (pending.load(std::memory_order_acquire) && hook) {
        to_call = std::move(hook);
      }
    }
    if (to_call) {
      to_call();
    }
  }

  void cancel() noexcept {
    pending.store(true, std::memory_order_release);

    unique_function<void()> to_call{};
    {
      std::scoped_lock lk{mtx};
      if (hook) {
        to_call = std::move(hook);
      }
    }

    if (to_call) {
      to_call();
    }
  }
};

/// Shared state for async operation awaiters.
/// Holds the coroutine handle, executor, and result error code.
struct operation_wait_state {
  std::coroutine_handle<> h{};
  any_executor ex{};
  std::error_code ec{};
  operation_cancellation cancel{};
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

  void publish_cancel(unique_function<void()> f) noexcept { st_->cancel.publish(std::move(f)); }

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
  requires std::derived_from<Operation, detail::async_operation>
struct operation_awaiter {
  std::unique_ptr<Operation> op;
  std::shared_ptr<operation_wait_state> st{std::make_shared<operation_wait_state>()};

  template <typename... Args>
  explicit operation_awaiter(Args&&... args) {
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

/// Adaptor: add cancellation_token semantics to an awaiter that uses operation_wait_state.
///
/// Requirements:
/// - Awaiter must expose `st` as `std::shared_ptr<operation_wait_state>`.
/// - Awaiter await_suspend must be safe to call exactly once (as usual for awaiters).
///
/// Semantics:
/// - Registers a token callback for the duration of the suspension.
/// - Token triggers `st->cancel.cancel()` (pending-cancel + hook publish handles races).
/// - If tok is already cancelled, this still starts the underlying operation; the already-fired
///   callback makes cancellation "pending" and it will be applied as soon as the operation
///   publishes its cancel hook.
template <class Awaiter>
struct cancellable_awaiter {
  Awaiter awaiter;
  cancellation_token tok{};
  cancellation_registration reg{};

  bool await_ready() const noexcept { return awaiter.await_ready(); }

  bool await_suspend(std::coroutine_handle<> h) {
    if (tok) {
      reg = tok.register_callback([st = awaiter.st]() { st->cancel.cancel(); });
    }

    return awaiter.await_suspend(h);
  }

  decltype(auto) await_resume() {
    // Ensure the callback is unregistered before returning to caller.
    reg.reset();
    return awaiter.await_resume();
  }
};

template <class Awaiter>
auto cancellable(Awaiter awaiter, cancellation_token tok) -> cancellable_awaiter<Awaiter> {
  return cancellable_awaiter<Awaiter>{std::move(awaiter), std::move(tok), {}};
}

}  // namespace iocoro::detail

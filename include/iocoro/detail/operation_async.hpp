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

/// Shared state for async operation awaiters.
/// Holds the coroutine handle, executor, and result error code.
struct operation_wait_state {
  std::coroutine_handle<> h{};
  any_executor ex{};
  std::error_code ec{};

  // Cancellation bridge:
  // - The awaiter may request cancellation via request_cancel().
  // - The reactor-facing operation publishes a cancel hook via set_cancel().
  //
  // This keeps cancellation_token out of reactor operations while still enabling
  // safe, race-free cancellation even if the cancel hook is published after the
  // token is cancelled (pending-cancel semantics).
  std::atomic<bool> cancel_pending{false};
  std::mutex cancel_mtx{};
  unique_function<void()> cancel_hook{};

  void set_cancel(unique_function<void()> f) noexcept {
    unique_function<void()> to_call{};
    {
      std::scoped_lock lk{cancel_mtx};
      cancel_hook = std::move(f);
      if (cancel_pending.load(std::memory_order_acquire) && cancel_hook) {
        to_call = std::move(cancel_hook);
      }
    }
    if (to_call) {
      to_call();
    }
  }

  void request_cancel() noexcept {
    cancel_pending.store(true, std::memory_order_release);

    unique_function<void()> to_call{};
    {
      std::scoped_lock lk{cancel_mtx};
      if (cancel_hook) {
        to_call = std::move(cancel_hook);
      }
    }

    if (to_call) {
      to_call();
    }
  }
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
  requires std::derived_from<Operation, detail::async_operation>
struct operation_awaiter {
  std::unique_ptr<Operation> op;
  std::shared_ptr<operation_wait_state> st{std::make_shared<operation_wait_state>()};
  cancellation_token tok{};
  cancellation_registration reg{};

  template <typename... Args>
  explicit operation_awaiter(cancellation_token tok_, Args&&... args) : tok(std::move(tok_)) {
    op = std::make_unique<Operation>(st, std::forward<Args>(args)...);
  }

  bool await_ready() const noexcept { return false; }

  bool await_suspend(std::coroutine_handle<> h) {
    st->h = h;
    st->ex = get_current_executor();

    if (tok) {
      // Register first to avoid TOCTOU gaps.
      reg = tok.register_callback([st = st]() { st->request_cancel(); });

      if (tok.stop_requested()) {
        st->ec = error::operation_aborted;
        return false;
      }
    }

    op->start(std::move(op));
    return true;
  }

  auto await_resume() noexcept -> std::error_code { return st->ec; }
};

}  // namespace iocoro::detail

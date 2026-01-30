#pragma once

#include <iocoro/assert.hpp>
#include <stop_token>
#include <iocoro/detail/executor_guard.hpp>
#include <iocoro/detail/unique_function.hpp>
#include <iocoro/detail/operation_base.hpp>
#include <iocoro/error.hpp>

#include <atomic>
#include <coroutine>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <system_error>

namespace iocoro::detail {

class io_context_impl;

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
/// This keeps stop_token out of reactor operations while still enabling race-free,
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

/// Async operation wrapper with self-owned registration.
class async_op {
 public:
  using register_fn = unique_function<void(async_op&, io_context_impl&, reactor_op_ptr)>;

  async_op(std::shared_ptr<operation_wait_state> st,
           io_context_impl* ctx,
           register_fn reg) noexcept
      : st_(std::move(st)), ctx_(ctx), reg_(std::move(reg)) {}

  void start() noexcept {
    IOCORO_ENSURE(ctx_ != nullptr, "async_op: null io_context_impl");
    IOCORO_ENSURE(reg_, "async_op: empty registration");
    auto op = make_reactor_op<state>(st_);
    reg_(*this, *ctx_, std::move(op));
  }

  void publish_cancel(unique_function<void()> f) noexcept { st_->cancel.publish(std::move(f)); }

 private:
  struct state {
    std::shared_ptr<operation_wait_state> st;
    std::atomic<bool> done{false};

    explicit state(std::shared_ptr<operation_wait_state> s) noexcept : st(std::move(s)) {}

    void on_complete() noexcept { complete(std::error_code{}); }
    void on_abort(std::error_code ec) noexcept { complete(ec); }

    void complete(std::error_code ec) noexcept {
      if (done.exchange(true, std::memory_order_acq_rel)) {
        return;
      }
      st->ec = ec;
      st->ex.post([st = st]() mutable { st->h.resume(); });
    }
  };

  std::shared_ptr<operation_wait_state> st_;
  io_context_impl* ctx_ = nullptr;
  register_fn reg_{};
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
///   co_await operation_awaiter{make_timer_wait_op(ctx_impl, timer)};
///   co_await operation_awaiter{make_fd_wait_op(ctx_impl, socket, kind)};
template <typename Factory>
struct operation_awaiter {
  Factory factory;
  std::shared_ptr<operation_wait_state> st{std::make_shared<operation_wait_state>()};
  std::optional<std::stop_callback<std::function<void()>>> reg{};

  explicit operation_awaiter(Factory f) : factory(std::move(f)) {}

  bool await_ready() const noexcept { return false; }

  template <class Promise>
  bool await_suspend(std::coroutine_handle<Promise> h) {
    st->h = h;
    st->ex = get_current_executor();
    if constexpr (requires { h.promise().get_stop_token(); }) {
      auto tok = h.promise().get_stop_token();
      if (tok.stop_possible()) {
        reg.emplace(std::move(tok), [st = st]() { st->cancel.cancel(); });
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

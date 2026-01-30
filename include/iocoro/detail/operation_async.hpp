#pragma once

#include <iocoro/assert.hpp>
#include <stop_token>
#include <iocoro/detail/executor_guard.hpp>
#include <iocoro/detail/io_context_impl.hpp>
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

/// Shared state for async operation awaiters.
/// Holds the coroutine handle, executor, and result error code.
struct operation_wait_state {
  std::coroutine_handle<> h{};
  any_executor ex{};
  std::error_code ec{};
};

/// Async operation wrapper with self-owned registration.
class async_op {
 public:
  using register_fn = unique_function<io_context_impl::event_handle(io_context_impl&, reactor_op_ptr)>;

  struct cancel_state {
    std::mutex mtx{};
    bool pending{false};
    io_context_impl::event_handle handle{};

    void set_handle(io_context_impl::event_handle h) noexcept {
      bool do_cancel = false;
      {
        std::scoped_lock lk{mtx};
        handle = std::move(h);
        do_cancel = pending;
      }
      if (do_cancel) {
        handle.cancel();
      }
    }

    void cancel() noexcept {
      io_context_impl::event_handle h{};
      {
        std::scoped_lock lk{mtx};
        if (!handle) {
          pending = true;
          return;
        }
        h = handle;
      }
      h.cancel();
    }
  };

  async_op(std::shared_ptr<operation_wait_state> st,
           io_context_impl* ctx,
           register_fn reg) noexcept
      : st_(std::move(st)), ctx_(ctx), reg_(std::move(reg)), cancel_(std::make_shared<cancel_state>()) {}

  void start() noexcept {
    IOCORO_ENSURE(ctx_ != nullptr, "async_op: null io_context_impl");
    IOCORO_ENSURE(reg_, "async_op: empty registration");
    auto op = make_reactor_op<state>(st_);
    auto handle = reg_(*ctx_, std::move(op));
    cancel_->set_handle(std::move(handle));
  }

  auto cancel_state_ptr() const noexcept -> std::shared_ptr<cancel_state> { return cancel_; }

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
  std::shared_ptr<cancel_state> cancel_{};
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

#pragma once

#include <iocoro/assert.hpp>
#include <iocoro/detail/io_context_impl.hpp>
#include <iocoro/detail/reactor_types.hpp>
#include <iocoro/detail/unique_function.hpp>

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

}  // namespace iocoro::detail

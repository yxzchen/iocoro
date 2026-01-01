#include <iocoro/assert.hpp>
#include <iocoro/detail/executor_guard.hpp>
#include <iocoro/detail/io_context_impl.hpp>
#include <iocoro/detail/operation_base.hpp>
#include <iocoro/error.hpp>
#include <iocoro/executor.hpp>
#include <iocoro/steady_timer.hpp>

#include <chrono>
#include <coroutine>
#include <memory>
#include <system_error>
#include <utility>

namespace iocoro {

inline steady_timer::steady_timer(io_executor ex) noexcept
    : ctx_impl_(ex.impl_), expiry_(clock::now()) {}

inline steady_timer::steady_timer(io_executor ex, time_point at) noexcept
    : ctx_impl_(ex.impl_), expiry_(at) {}

inline steady_timer::steady_timer(io_executor ex, duration after) noexcept
    : ctx_impl_(ex.impl_), expiry_(clock::now() + after) {}

inline steady_timer::~steady_timer() {
  (void)cancel();
}

inline auto steady_timer::expires_at(time_point at) noexcept -> std::size_t {
  expiry_ = at;
  return cancel();
}

inline auto steady_timer::expires_after(duration d) noexcept -> std::size_t {
  expiry_ = clock::now() + d;
  return cancel();
}

inline auto steady_timer::cancel() noexcept -> std::size_t {
  if (handle_) {
    handle_.cancel();
    handle_ = detail::io_context_impl::timer_event_handle::invalid_handle();
    return 1;
  }
  return 0;
}

inline auto steady_timer::async_wait(use_awaitable_t) -> awaitable<std::error_code> {
  using namespace std::chrono;

  // Calculate timeout
  auto const now = clock::now();
  auto d = (expiry_ <= now) ? duration::zero() : (expiry_ - now);
  auto ms = ceil<milliseconds>(d);
  if (ms.count() < 0) {
    ms = milliseconds{0};
  }

  // State shared between awaiter and operation
  struct wait_state final {
    std::coroutine_handle<> h{};
    any_executor ex{};
    std::error_code ec{};
  };

  // Timer operation
  class timer_wait_operation final
      : public detail::operation_base
      , private detail::one_shot_completion {
   public:
    timer_wait_operation(steady_timer* timer, std::shared_ptr<wait_state> st)
        : operation_base(timer->ctx_impl_), timer_(timer), st_(std::move(st)) {}

    void on_ready() noexcept override {
      complete(std::error_code{});
    }

    void on_abort(std::error_code ec) noexcept override {
      complete(ec);
    }

   private:
    void do_start(std::unique_ptr<operation_base> self) override {
      auto handle = impl_->schedule_timer(timer_->expiry(), std::move(self));
      timer_->set_write_handle(handle);
    }

    void complete(std::error_code ec) {
      // Guard against double completion (on_ready + on_abort, or repeated signals).
      if (!try_complete()) {
        return;
      }
      st_->ec = ec;

      // Resume on the caller's original executor (if set).
      // If no executor, resume directly in the current context.
      st_->ex.post([st = st_]() mutable { st->h.resume(); });
    }

    steady_timer* timer_ = nullptr;
    std::shared_ptr<wait_state> st_;
  };

  struct timer_awaiter final {
    steady_timer* self;
    std::shared_ptr<wait_state> st{};

    bool await_ready() const noexcept { return false; }

    void await_suspend(std::coroutine_handle<> h) {
      st = std::make_shared<wait_state>();
      st->h = h;
      st->ex = detail::get_current_executor();

      // Create and register timer operation
      auto op = std::make_unique<timer_wait_operation>(self, st);
      op->start(std::move(op));
    }

    auto await_resume() noexcept -> std::error_code {
      return st->ec;
    }
  };

  co_return co_await timer_awaiter{this};
}

}  // namespace iocoro

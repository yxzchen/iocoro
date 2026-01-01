#include <iocoro/assert.hpp>
#include <iocoro/detail/async_operation.hpp>
#include <iocoro/detail/executor_guard.hpp>
#include <iocoro/detail/io_context_impl.hpp>
#include <iocoro/detail/operation_awaiter.hpp>
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

  // Timer operation
  class timer_wait_operation final : public detail::async_operation {
   public:
    timer_wait_operation(std::shared_ptr<detail::operation_wait_state> st, steady_timer* timer)
        : async_operation(std::move(st), timer->ctx_impl_), timer_(timer) {}

   private:
    void do_start(std::unique_ptr<operation_base> self) override {
      auto handle = this->impl_->schedule_timer(timer_->expiry(), std::move(self));
      timer_->set_write_handle(handle);
    }

    steady_timer* timer_ = nullptr;
  };

  co_return co_await detail::operation_awaiter<timer_wait_operation, steady_timer*>{this};
}

}  // namespace iocoro

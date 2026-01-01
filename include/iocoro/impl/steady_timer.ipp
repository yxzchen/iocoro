#include <iocoro/assert.hpp>
#include <iocoro/detail/executor_guard.hpp>
#include <iocoro/detail/io_context_impl.hpp>
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

inline steady_timer::~steady_timer() { (void)cancel(); }

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

}  // namespace iocoro

#include <iocoro/assert.hpp>
#include <iocoro/detail/io_context_impl.hpp>
#include <iocoro/detail/reactor_backend.hpp>
#include <iocoro/detail/reactor_types.hpp>
#include <iocoro/error.hpp>

#ifdef IOCORO_USE_URING
#include <iocoro/impl/backends/uring.ipp>
#else
#include <iocoro/impl/backends/epoll.ipp>
#endif

#include <algorithm>
#include <chrono>
#include <utility>

namespace iocoro::detail {

inline auto io_context_impl::this_thread_token() noexcept -> std::uintptr_t {
  static thread_local int tls_anchor = 0;
  return reinterpret_cast<std::uintptr_t>(&tls_anchor);
}

inline io_context_impl::io_context_impl() : backend_(make_backend()) {}

inline io_context_impl::io_context_impl(std::unique_ptr<backend_interface> backend)
    : backend_(std::move(backend)) {
  IOCORO_ENSURE(backend_ != nullptr, "io_context_impl: null backend");
}

inline io_context_impl::~io_context_impl() {
  stop();
  backend_.reset();
}

inline void io_context_impl::set_thread_id() noexcept {
  thread_token_.store(this_thread_token(), std::memory_order_release);
}

inline auto io_context_impl::running_in_this_thread() const noexcept -> bool {
  return thread_token_.load(std::memory_order_acquire) == this_thread_token();
}

inline auto io_context_impl::run() -> std::size_t {
  set_thread_id();

  std::size_t count = 0;
  while (!stopped_.load(std::memory_order_acquire) && has_work()) {
    count += process_posted();
    count += process_timers();

    if (stopped_.load(std::memory_order_acquire) || !has_work()) {
      break;
    }

    count += process_events(get_timeout());
  }
  return count;
}

inline auto io_context_impl::run_one() -> std::size_t {
  set_thread_id();

  std::size_t count = 0;
  count += process_posted();
  if (count > 0) {
    return count;
  }

  count += process_timers();
  if (count > 0) {
    return count;
  }

  if (stopped_.load(std::memory_order_acquire) || !has_work()) {
    return 0;
  }
  return process_events(get_timeout());
}

inline auto io_context_impl::run_for(std::chrono::milliseconds timeout) -> std::size_t {
  set_thread_id();

  auto const deadline = std::chrono::steady_clock::now() + timeout;
  std::size_t count = 0;

  while (!stopped_.load(std::memory_order_acquire) && has_work()) {
    auto const now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      break;
    }

    count += process_posted();
    count += process_timers();

    if (stopped_.load(std::memory_order_acquire) || !has_work()) {
      break;
    }

    auto const remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::max(deadline - now, std::chrono::steady_clock::duration::zero()));

    std::chrono::milliseconds wait_ms = remaining;
    auto const timer_timeout = get_timeout();
    if (timer_timeout) {
      wait_ms = std::min(wait_ms, *timer_timeout);
    }

    count += process_events(wait_ms);
  }
  return count;
}

inline void io_context_impl::stop() {
  stopped_.store(true, std::memory_order_release);
  wakeup();
}

inline void io_context_impl::restart() { stopped_.store(false, std::memory_order_release); }

inline void io_context_impl::post(unique_function<void()> f) {
  posted_.post(std::move(f));
  wakeup();
}

inline void io_context_impl::dispatch(unique_function<void()> f) {
  if (running_in_this_thread() && !stopped_.load(std::memory_order_acquire)) {
    f();
  } else {
    post(std::move(f));
  }
}

inline auto io_context_impl::add_timer(std::chrono::steady_clock::time_point expiry,
                                       reactor_op_ptr op) -> event_handle {
  auto token = timers_.add_timer(expiry, std::move(op));
  auto h = event_handle::make_timer(this, token.index, token.generation);
  wakeup();
  return h;
}

inline void io_context_impl::cancel_timer(std::uint32_t index, std::uint32_t generation) noexcept {
  if (timers_.cancel(timer_registry::timer_token{index, generation})) {
    wakeup();
  }
}

inline auto io_context_impl::register_fd_read(int fd, reactor_op_ptr op) -> event_handle {
  auto result = fd_registry_.register_read(fd, std::move(op));
  if (result.replaced) {
    result.replaced->vt->on_abort(result.replaced->block, error::operation_aborted);
  }
  apply_fd_interest(fd, result.interest);
  wakeup();
  if (result.token == 0) {
    return event_handle::invalid_handle();
  }
  return event_handle::make_fd(this, fd, detail::fd_event_kind::read, result.token);
}

inline auto io_context_impl::register_fd_write(int fd, reactor_op_ptr op) -> event_handle {
  auto result = fd_registry_.register_write(fd, std::move(op));
  if (result.replaced) {
    result.replaced->vt->on_abort(result.replaced->block, error::operation_aborted);
  }
  apply_fd_interest(fd, result.interest);
  wakeup();
  if (result.token == 0) {
    return event_handle::invalid_handle();
  }
  return event_handle::make_fd(this, fd, detail::fd_event_kind::write, result.token);
}

inline void io_context_impl::deregister_fd(int fd) {
  auto removed = fd_registry_.deregister(fd);
  if (removed.had_any) {
    apply_fd_interest(fd, removed.interest);
  }

  if (removed.read) {
    removed.read->vt->on_abort(removed.read->block, error::operation_aborted);
  }
  if (removed.write) {
    removed.write->vt->on_abort(removed.write->block, error::operation_aborted);
  }
  wakeup();
}

inline void io_context_impl::cancel_fd_event(int fd, detail::fd_event_kind kind,
                                             std::uint64_t token) noexcept {
  auto result = fd_registry_.cancel(fd, kind, token);
  if (!result.matched) {
    return;
  }

  apply_fd_interest(fd, result.interest);
  if (result.removed) {
    result.removed->vt->on_abort(result.removed->block, error::operation_aborted);
  }
  wakeup();
}

inline void io_context_impl::cancel_event(event_handle h) noexcept {
  if (!h) {
    return;
  }
  if (h.type == event_handle::kind::fd) {
    cancel_fd_event(h.fd, h.fd_kind, h.token);
    return;
  }
  if (h.type == event_handle::kind::timer) {
    cancel_timer(h.timer_index, h.timer_generation);
  }
}

inline void io_context_impl::add_work_guard() noexcept {
  posted_.add_work_guard();
}

inline void io_context_impl::remove_work_guard() noexcept {
  auto const old = posted_.remove_work_guard();
  if (old == 1) {
    wakeup();
  }
}

inline auto io_context_impl::process_timers() -> std::size_t {
  return timers_.process_expired(stopped_.load(std::memory_order_acquire));
}

inline auto io_context_impl::process_posted() -> std::size_t {
  return posted_.process(stopped_.load(std::memory_order_acquire));
}

inline auto io_context_impl::get_timeout() -> std::optional<std::chrono::milliseconds> {
  return timers_.next_timeout();
}

inline auto io_context_impl::has_work() -> bool {
  if (posted_.work_guard_count() > 0) {
    return true;
  }
  if (!posted_.empty()) {
    return true;
  }
  if (!timers_.empty()) {
    return true;
  }
  if (!fd_registry_.empty()) {
    return true;
  }
  return false;
}

inline auto io_context_impl::process_events(std::optional<std::chrono::milliseconds> max_wait)
  -> std::size_t {
  backend_events_.clear();
  backend_->wait(max_wait, backend_events_);
  std::size_t count = 0;

  for (auto const& ev : backend_events_) {
    if (ev.fd < 0) {
      continue;
    }

    auto ready = fd_registry_.take_ready(ev.fd, ev.can_read, ev.can_write);
    apply_fd_interest(ev.fd, ready.interest);

    if (ev.is_error) {
      if (ready.ops.read) {
        ready.ops.read->vt->on_abort(ready.ops.read->block, ev.ec);
        ++count;
      }
      if (ready.ops.write) {
        ready.ops.write->vt->on_abort(ready.ops.write->block, ev.ec);
        ++count;
      }
    } else {
      if (ready.ops.read) {
        ready.ops.read->vt->on_complete(ready.ops.read->block);
        ++count;
      }
      if (ready.ops.write) {
        ready.ops.write->vt->on_complete(ready.ops.write->block);
        ++count;
      }
    }
  }
  return count;
}

inline void io_context_impl::wakeup() { backend_->wakeup(); }

inline void io_context_impl::apply_fd_interest(int fd, fd_interest interest) {
  if (interest.want_read || interest.want_write) {
    backend_->update_fd_interest(fd, interest.want_read, interest.want_write);
  } else {
    backend_->remove_fd_interest(fd);
  }
}

inline void event_handle::cancel() const noexcept {
  if (!valid()) {
    return;
  }
  impl->cancel_event(*this);
}

}  // namespace iocoro::detail

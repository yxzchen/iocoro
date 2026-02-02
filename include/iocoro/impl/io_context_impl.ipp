#include <iocoro/assert.hpp>
#include <iocoro/detail/io_context_impl.hpp>
#include <iocoro/detail/reactor_backend.hpp>
#include <iocoro/detail/reactor_types.hpp>
#include <iocoro/detail/scope_guard.hpp>
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

inline auto io_context_impl::is_stopped() const noexcept -> bool {
  return stopped_.load(std::memory_order_acquire);
}

inline auto io_context_impl::run() -> std::size_t {
  IOCORO_ENSURE(!running_.exchange(true, std::memory_order_acq_rel),
               "io_context_impl::run(): concurrent event loops are not supported");
  auto running_guard =
    detail::make_scope_exit([this]() noexcept { running_.store(false, std::memory_order_release); });
  set_thread_id();

  std::size_t count = 0;
  while (!is_stopped() && has_work()) {
    count += process_posted();
    count += process_timers();
    if (is_stopped() || !has_work()) {
      break;
    }
    auto wait = next_wait(std::nullopt);
    if (posted_.has_pending_tasks()) {
      wait = std::chrono::milliseconds{0};
    }
    count += process_events(wait);
  }
  return count;
}

inline auto io_context_impl::run_one() -> std::size_t {
  IOCORO_ENSURE(!running_.exchange(true, std::memory_order_acq_rel),
               "io_context_impl::run_one(): concurrent event loops are not supported");
  auto running_guard =
    detail::make_scope_exit([this]() noexcept { running_.store(false, std::memory_order_release); });
  set_thread_id();

  std::size_t count = process_posted();
  if (count > 0) {
    return count;
  }

  count += process_timers();
  if (count > 0) {
    return count;
  }

  if (is_stopped() || !has_work()) {
    return 0;
  }
  return process_events(next_wait(std::nullopt));
}

inline auto io_context_impl::run_for(std::chrono::milliseconds timeout) -> std::size_t {
  IOCORO_ENSURE(!running_.exchange(true, std::memory_order_acq_rel),
               "io_context_impl::run_for(): concurrent event loops are not supported");
  auto running_guard =
    detail::make_scope_exit([this]() noexcept { running_.store(false, std::memory_order_release); });
  set_thread_id();

  auto const deadline = std::chrono::steady_clock::now() + timeout;
  std::size_t count = 0;

  while (!is_stopped() && has_work()) {
    if (std::chrono::steady_clock::now() >= deadline) {
      break;
    }

    count += process_posted();
    count += process_timers();
    if (is_stopped() || !has_work()) {
      break;
    }

    auto wait = next_wait(deadline);
    if (posted_.has_pending_tasks()) {
      wait = std::chrono::milliseconds{0};
    }
    count += process_events(wait);
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
  if (!running_in_this_thread()) {
    wakeup();
  }
}

inline void io_context_impl::dispatch(unique_function<void()> f) {
  if (running_in_this_thread() && !is_stopped()) {
    f();
  } else {
    post(std::move(f));
  }
}

inline void io_context_impl::dispatch_reactor(unique_function<void(io_context_impl&)> f) noexcept {
  // Reactor invariant: registry/backend mutations and op callbacks must occur on the reactor thread.
  // If the loop is not running yet, we still enqueue so that the next run()/run_one()
  // establishes the reactor thread and drains the callback there.
  if (running_in_this_thread()) {
    if (f) {
      f(*this);
    }
    return;
  }

  // Avoid a self-owning cycle: do NOT capture shared_ptr in posted tasks.
  // Instead capture weak_ptr and lock at execution time to pin lifetime during the callback.
  auto weak = weak_from_this();
  IOCORO_ENSURE(!weak.expired(),
               "io_context_impl::dispatch_reactor(): io_context_impl must be shared-owned "
               "(construct with std::make_shared<io_context_impl>())");
  post([weak = std::move(weak), f = std::move(f)]() mutable noexcept {
    auto self = weak.lock();
    if (!self) {
      return;
    }
    if (f) {
      f(*self);
    }
  });
}

inline void io_context_impl::abort_op(reactor_op_ptr op, std::error_code ec) noexcept {
  if (!op) {
    return;
  }
  op->vt->on_abort(op->block, ec);
}

inline auto io_context_impl::add_timer(std::chrono::steady_clock::time_point expiry,
                                       reactor_op_ptr op) -> event_handle {
  // When the event loop is active, registry/backend ownership is the reactor thread.
  // Before the loop starts, we allow single-threaded setup on the caller thread.
  if (running_.load(std::memory_order_acquire)) {
    IOCORO_ENSURE(running_in_this_thread(),
                 "io_context_impl::add_timer(): must run on io_context thread");
  }
  auto token = timers_.add_timer(expiry, std::move(op));
  auto h = event_handle::make_timer(weak_from_this(), token.index, token.generation);
  wakeup();
  return h;
}

inline void io_context_impl::cancel_timer(std::uint32_t index, std::uint64_t generation) noexcept {
  // Thread-safe entrypoint: always route cancellation to the reactor thread so that
  // registry mutation and abort callbacks occur in a single-threaded context.
  dispatch_reactor([index, generation](io_context_impl& self) mutable noexcept {
    auto res = self.timers_.cancel(timer_registry::timer_token{index, generation});
    abort_op(std::move(res.op), error::operation_aborted);
    if (res.cancelled) {
      self.wakeup();
    }
  });
}

inline auto io_context_impl::register_fd_read(int fd, reactor_op_ptr op) -> event_handle {
  if (running_.load(std::memory_order_acquire)) {
    IOCORO_ENSURE(running_in_this_thread(),
                 "io_context_impl::register_fd_read(): must run on io_context thread");
  }
  auto result = fd_registry_.register_read(fd, std::move(op));
  abort_op(std::move(result.replaced), error::operation_aborted);
  wakeup();
  if (result.token == 0) {
    return event_handle::invalid_handle();
  }
  return event_handle::make_fd(weak_from_this(), fd, detail::fd_event_kind::read, result.token);
}

inline auto io_context_impl::register_fd_write(int fd, reactor_op_ptr op) -> event_handle {
  if (running_.load(std::memory_order_acquire)) {
    IOCORO_ENSURE(running_in_this_thread(),
                 "io_context_impl::register_fd_write(): must run on io_context thread");
  }
  auto result = fd_registry_.register_write(fd, std::move(op));
  abort_op(std::move(result.replaced), error::operation_aborted);
  wakeup();
  if (result.token == 0) {
    return event_handle::invalid_handle();
  }
  return event_handle::make_fd(weak_from_this(), fd, detail::fd_event_kind::write, result.token);
}

inline auto io_context_impl::arm_fd_interest(int fd) noexcept -> result<void> {
  if (fd < 0) {
    return fail(error::invalid_argument);
  }
  try {
    apply_fd_interest(fd, fd_interest{true, true});
  } catch (...) {
    return fail(error::internal_error);
  }
  wakeup();
  return ok();
}

inline void io_context_impl::disarm_fd_interest(int fd) noexcept {
  if (fd < 0) {
    return;
  }
  backend_->remove_fd_interest(fd);
}

inline void io_context_impl::deregister_fd(int fd) {
  if (running_.load(std::memory_order_acquire)) {
    IOCORO_ENSURE(running_in_this_thread(),
                 "io_context_impl::deregister_fd(): must run on io_context thread");
  }
  auto removed = fd_registry_.deregister(fd);
  abort_op(std::move(removed.read), error::operation_aborted);
  abort_op(std::move(removed.write), error::operation_aborted);
  wakeup();
}

inline void io_context_impl::cancel_fd_event(int fd, detail::fd_event_kind kind,
                                             std::uint64_t token) noexcept {
  // Thread-safe entrypoint: always route cancellation to the reactor thread so that
  // registry mutation and abort callbacks occur in a single-threaded context.
  dispatch_reactor([fd, kind, token](io_context_impl& self) mutable noexcept {
    auto result = self.fd_registry_.cancel(fd, kind, token);
    if (!result.matched) {
      return;
    }
    abort_op(std::move(result.removed), error::operation_aborted);
    self.wakeup();
  });
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
  IOCORO_ENSURE(running_in_this_thread(),
               "io_context_impl::process_timers(): must run on io_context thread");
  return timers_.process_expired(is_stopped());
}

inline auto io_context_impl::process_posted() -> std::size_t {
  IOCORO_ENSURE(running_in_this_thread(),
               "io_context_impl::process_posted(): must run on io_context thread");
  return posted_.process(is_stopped());
}

inline auto io_context_impl::next_wait(std::optional<std::chrono::steady_clock::time_point> deadline)
  -> std::optional<std::chrono::milliseconds> {
  auto const timer_timeout = timers_.next_timeout();
  if (!deadline) {
    return timer_timeout;
  }
  auto const now = std::chrono::steady_clock::now();
  if (now >= *deadline) {
    return std::chrono::milliseconds(0);
  }
  auto const remaining = std::chrono::duration_cast<std::chrono::milliseconds>(*deadline - now);
  if (!timer_timeout) {
    return remaining;
  }
  return std::min(remaining, *timer_timeout);
}

inline auto io_context_impl::has_work() -> bool {
  return posted_.has_work() || !timers_.empty() || !fd_registry_.empty();
}

inline auto io_context_impl::process_events(std::optional<std::chrono::milliseconds> max_wait)
  -> std::size_t {
  IOCORO_ENSURE(running_in_this_thread(),
               "io_context_impl::process_events(): must run on io_context thread");
  backend_->wait(max_wait, backend_events_);
  std::size_t count = 0;

  auto process_one = [&](reactor_op_ptr& op, bool is_error, std::error_code ec) -> void {
    if (!op) {
      return;
    }
    if (is_error) {
      op->vt->on_abort(op->block, ec);
    } else {
      op->vt->on_complete(op->block);
    }
    ++count;
  };

  for (auto const& ev : backend_events_) {
    if (ev.fd < 0) {
      continue;
    }

    auto ready = fd_registry_.take_ready(ev.fd, ev.can_read, ev.can_write);
    process_one(ready.read, ev.is_error, ev.ec);
    process_one(ready.write, ev.is_error, ev.ec);
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
  auto s = impl.lock();
  if (!s) {
    return;
  }
  s->cancel_event(*this);
}

}  // namespace iocoro::detail

#include <iocoro/assert.hpp>
#include <iocoro/detail/io_context_impl.hpp>
#include <iocoro/detail/reactor_backend.hpp>
#include <iocoro/detail/reactor_types.hpp>
#include <iocoro/detail/scope_guard.hpp>
#include <iocoro/error.hpp>

// Backend selection for header-only builds.
//
// Default: epoll (no extra dependencies).
//
// To force io_uring backend (requires liburing headers + linking `-luring`):
// - Define `IOCORO_BACKEND_URING`.
//
// To force epoll explicitly:
// - Define `IOCORO_BACKEND_EPOLL`.
//
// IMPORTANT: Include only the selected backend implementation.
// Both backends define internal helpers in anonymous namespaces; including both in the same TU
// would cause redefinition errors.

#if !defined(__linux__)
#error "iocoro currently supports Linux backends only"
#endif

#if defined(IOCORO_BACKEND_EPOLL)
#include <iocoro/impl/backends/epoll.ipp>
#elif defined(IOCORO_BACKEND_URING)
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
  return running_.load(std::memory_order_acquire) &&
         thread_token_.load(std::memory_order_acquire) == this_thread_token();
}

inline auto io_context_impl::is_stopped() const noexcept -> bool {
  return stopped_.load(std::memory_order_acquire);
}

inline auto io_context_impl::run() -> std::size_t {
  IOCORO_ENSURE(!running_.exchange(true, std::memory_order_acq_rel),
                "io_context_impl::run(): concurrent event loops are not supported");
  auto running_guard = detail::make_scope_exit([this]() noexcept {
    thread_token_.store(0, std::memory_order_release);
    running_.store(false, std::memory_order_release);
  });
  set_thread_id();

  std::size_t count = 0;
  while (true) {
    if (is_stopped() || !has_work()) {
      break;
    }
    count += process_posted();
    if (is_stopped() || !has_work()) {
      break;
    }
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
  auto running_guard = detail::make_scope_exit([this]() noexcept {
    thread_token_.store(0, std::memory_order_release);
    running_.store(false, std::memory_order_release);
  });
  set_thread_id();

  if (is_stopped() || !has_work()) {
    return 0;
  }

  std::size_t count;
  if (count = process_posted(); count > 0) {
    return count;
  }

  if (count = process_timers(); count > 0) {
    return count;
  }

  return process_events(next_wait(std::nullopt));
}

inline auto io_context_impl::run_for(std::chrono::milliseconds timeout) -> std::size_t {
  IOCORO_ENSURE(!running_.exchange(true, std::memory_order_acq_rel),
                "io_context_impl::run_for(): concurrent event loops are not supported");
  auto running_guard = detail::make_scope_exit([this]() noexcept {
    thread_token_.store(0, std::memory_order_release);
    running_.store(false, std::memory_order_release);
  });
  set_thread_id();

  auto const deadline = std::chrono::steady_clock::now() + timeout;
  std::size_t count = 0;

  while (true) {
    if (std::chrono::steady_clock::now() >= deadline) {
      break;
    }

    if (is_stopped() || !has_work()) {
      break;
    }

    count += process_posted();
    if (is_stopped() || !has_work()) {
      break;
    }

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

inline void io_context_impl::restart() {
  stopped_.store(false, std::memory_order_release);
}

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
  // INVARIANT: registry/backend mutations and op callbacks occur on the reactor thread.
  // If the loop is not running yet, we still enqueue so that the next run()/run_one()
  // establishes the reactor thread and drains the callback there.
  if (running_in_this_thread()) {
    if (f) {
      f(*this);
    }
    return;
  }

  // SAFETY: avoid self-owning cycles (posted task -> shared_ptr -> impl -> posted queue).
  // Capture `weak_ptr` and lock at execution time to pin lifetime only during the callback.
  auto weak = weak_from_this();
  IOCORO_ENSURE(!weak.expired(),
                "io_context_impl::dispatch_reactor(): io_context_impl must be shared-owned "
                "(construct with std::make_shared<io_context_impl>())");
  post([weak = std::move(weak), f = std::move(f)]() mutable {
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
  // IMPORTANT: Once the loop is running, registry/backend is owned by the reactor thread.
  // Before the loop starts, we allow single-threaded setup by the caller.
  if (running_.load(std::memory_order_acquire)) {
    IOCORO_ENSURE(running_in_this_thread(),
                  "io_context_impl::add_timer(): must run on io_context thread");
  }
  auto token = timers_.add_timer(expiry, std::move(op));
  auto h = event_handle::make_timer(weak_from_this(), token.index, token.generation);
  return h;
}

inline void io_context_impl::cancel_timer(std::uint32_t index, std::uint64_t generation) noexcept {
  // Thread-safe entrypoint: always route cancellation to the reactor thread so that
  // registry mutation and abort callbacks occur in a single-threaded context.
  dispatch_reactor([index, generation](io_context_impl& self) mutable noexcept {
    auto res = self.timers_.cancel(timer_registry::timer_token{index, generation});
    abort_op(std::move(res.op), error::operation_aborted);
  });
}

inline auto io_context_impl::register_fd_read(int fd, reactor_op_ptr op) -> event_handle {
  return register_fd_impl(fd, std::move(op), detail::fd_event_kind::read);
}

inline auto io_context_impl::register_fd_write(int fd, reactor_op_ptr op) -> event_handle {
  return register_fd_impl(fd, std::move(op), detail::fd_event_kind::write);
}

inline auto io_context_impl::register_fd_impl(int fd, reactor_op_ptr op,
                                              detail::fd_event_kind kind) -> event_handle {
  if (running_.load(std::memory_order_acquire)) {
    IOCORO_ENSURE(running_in_this_thread(),
                  "io_context_impl::register_fd_*(): must run on "
                  "io_context thread");
  }
  auto result = (kind == detail::fd_event_kind::read)
                  ? fd_registry_.register_read(fd, std::move(op))
                  : fd_registry_.register_write(fd, std::move(op));
  abort_op(std::move(result.replaced), error::operation_aborted);
  if (result.ready_now) {
    result.ready_now->vt->on_complete(result.ready_now->block);
  }
  if (result.token == invalid_token) {
    return event_handle::invalid_handle();
  }
  return event_handle::make_fd(weak_from_this(), fd, kind, result.token);
}

inline void io_context_impl::remove_fd_impl(int fd) noexcept {
  auto removed = fd_registry_.deregister(fd);
  abort_op(std::move(removed.read), error::operation_aborted);
  abort_op(std::move(removed.write), error::operation_aborted);
  backend_->remove_fd(fd);
}

inline auto io_context_impl::add_fd(int fd) noexcept -> bool {
  if (fd < 0) {
    return false;
  }
  try {
    backend_->add_fd(fd);
  } catch (...) {
    return false;
  }
  if (running_.load(std::memory_order_acquire) && !running_in_this_thread()) {
    dispatch_reactor([fd](io_context_impl& self) noexcept { self.fd_registry_.track(fd); });
  } else {
    fd_registry_.track(fd);
  }
  wakeup();
  return true;
}

inline void io_context_impl::remove_fd(int fd) noexcept {
  if (fd < 0) {
    return;
  }

  if (running_.load(std::memory_order_acquire) && !running_in_this_thread()) {
    dispatch_reactor([fd](io_context_impl& self) noexcept { self.remove_fd_impl(fd); });
    return;
  }

  remove_fd_impl(fd);
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
  work_guard_.add();
}

inline void io_context_impl::remove_work_guard() noexcept {
  auto const old = work_guard_.remove();
  if (old == 1) {
    wakeup();
  }
}

inline auto io_context_impl::process_timers() -> std::size_t {
  IOCORO_ENSURE(running_in_this_thread(),
                "io_context_impl::process_timers(): must run on io_context thread");
  return timers_.process_expired();
}

inline auto io_context_impl::process_posted() -> std::size_t {
  IOCORO_ENSURE(running_in_this_thread(),
                "io_context_impl::process_posted(): must run on io_context thread");
  return posted_.process();
}

inline auto io_context_impl::next_wait(std::optional<std::chrono::steady_clock::time_point>
                                         deadline) -> std::optional<std::chrono::milliseconds> {
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
  return work_guard_.has_work() || posted_.has_pending_tasks() || !timers_.empty() ||
         !fd_registry_.empty();
}

inline auto io_context_impl::process_events(std::optional<std::chrono::milliseconds> max_wait)
  -> std::size_t {
  IOCORO_ENSURE(running_in_this_thread(),
                "io_context_impl::process_events(): must run on io_context thread");
  try {
    backend_->wait(max_wait, backend_events_);
  } catch (...) {
    // Backend failure is treated as a fatal internal error for this io_context instance.
    // Abort all in-flight reactor operations so awaiters can observe an error rather than
    // hanging indefinitely.
    std::error_code const ec = error::internal_error;

    stopped_.store(true, std::memory_order_release);

    auto drained = fd_registry_.drain_all();
    for (int fd : drained.fds) {
      backend_->remove_fd(fd);
    }
    for (auto& op : drained.ops) {
      abort_op(std::move(op), ec);
    }

    auto timer_ops = timers_.drain_all();
    for (auto& op : timer_ops) {
      abort_op(std::move(op), ec);
    }

    // Best-effort: drain posted tasks, swallowing user callback exceptions.
    while (posted_.has_pending_tasks()) {
      try {
        (void)process_posted();
      } catch (...) {
      }
    }
    return 0;
  }
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

inline void io_context_impl::wakeup() {
  backend_->wakeup();
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

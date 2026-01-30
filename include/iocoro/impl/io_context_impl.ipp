#include <iocoro/assert.hpp>
#include <iocoro/detail/io_context_impl.hpp>
#include <iocoro/detail/reactor_op.hpp>
#include <iocoro/error.hpp>

#ifdef IOCORO_USE_URING
#include <iocoro/impl/backends/uring.ipp>
#else
#include <iocoro/impl/backends/epoll.ipp>
#endif

#include <algorithm>
#include <chrono>
#include <queue>
#include <utility>

namespace iocoro::detail {

inline auto io_context_impl::this_thread_token() noexcept -> std::uintptr_t {
  // Each thread gets its own instance of this object; its address is stable and unique
  // among concurrently running threads, and avoids non-portable thread-id atomics.
  static thread_local int tls_anchor = 0;
  return reinterpret_cast<std::uintptr_t>(&tls_anchor);
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
  {
    std::scoped_lock lk{posted_mutex_};
    posted_operations_.push(std::move(f));
  }
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
                                       reactor_op_ptr op)
  -> timer_event_handle {
  auto entry = std::make_shared<detail::timer_entry>();
  entry->expiry = expiry;
  entry->op = std::move(op);
  entry->state.store(timer_state::pending, std::memory_order_release);

  {
    std::scoped_lock lk{timer_mutex_};
    entry->id = next_timer_id_++;
    timers_.push(entry);
  }

  wakeup();
  return timer_event_handle{this, entry};
}

inline void io_context_impl::cancel_timer(timer_event_handle h) noexcept {
  h.cancel();
}

inline auto io_context_impl::register_event(event_desc desc, reactor_op_ptr op) -> event_handle {
  switch (desc.type) {
    case event_desc::kind::timer: {
      return event_handle{add_timer(desc.expiry, std::move(op))};
    }
    case event_desc::kind::fd_read: {
      return event_handle{register_fd_read(desc.fd, std::move(op))};
    }
    case event_desc::kind::fd_write: {
      return event_handle{register_fd_write(desc.fd, std::move(op))};
    }
  }
  return event_handle{};
}

inline auto io_context_impl::register_fd_read(int fd, reactor_op_ptr op)
  -> fd_event_handle {
  reactor_op_ptr old;
  std::uint64_t token = 0;

  {
    std::scoped_lock lk{fd_mutex_};
    auto it = fd_operations_.find(fd);
    if (it == fd_operations_.end() && !op) {
      return fd_event_handle::invalid_handle();
    }

    if (it == fd_operations_.end()) {
      it = fd_operations_.emplace(fd, fd_ops{}).first;
    }

    auto& ops = it->second;
    if (op) {
      token = ops.read_token = next_fd_token_++;
    }

    old = std::exchange(ops.read_op, std::move(op));

    if (!ops.read_op && !ops.write_op) {
      fd_operations_.erase(it);
    }
  }
  if (old) {
    old->vt->on_abort(old->block, error::operation_aborted);
  }

  dispatch([this, fd] { reconcile_fd_interest(fd); });

  wakeup();
  return fd_event_handle{this, fd, detail::fd_event_kind::read, token};
}

inline auto io_context_impl::register_fd_write(int fd, reactor_op_ptr op)
  -> fd_event_handle {
  reactor_op_ptr old;
  std::uint64_t token = 0;

  {
    std::scoped_lock lk{fd_mutex_};
    auto it = fd_operations_.find(fd);
    if (it == fd_operations_.end() && !op) {
      return fd_event_handle::invalid_handle();
    }

    if (it == fd_operations_.end()) {
      it = fd_operations_.emplace(fd, fd_ops{}).first;
    }

    auto& ops = it->second;
    if (op) {
      token = ops.write_token = next_fd_token_++;
    }

    old = std::exchange(ops.write_op, std::move(op));

    if (!ops.read_op && !ops.write_op) {
      fd_operations_.erase(it);
    }
  }
  if (old) {
    old->vt->on_abort(old->block, error::operation_aborted);
  }

  dispatch([this, fd] { reconcile_fd_interest(fd); });

  wakeup();
  return fd_event_handle{this, fd, detail::fd_event_kind::write, token};
}

inline void io_context_impl::deregister_fd(int fd) {
  fd_ops removed;
  {
    std::scoped_lock lk{fd_mutex_};
    auto it = fd_operations_.find(fd);
    if (it == fd_operations_.end()) {
      // Ensure any stale backend registration is removed, but do it out of the lock.
    } else {
      removed = std::move(it->second);
      fd_operations_.erase(it);
    }
  }

  // Always attempt to remove interest; safe and idempotent if it wasn't registered.
  dispatch([this, fd] { reconcile_fd_interest(fd); });

  if (removed.read_op) {
    removed.read_op->vt->on_abort(removed.read_op->block, error::operation_aborted);
  }
  if (removed.write_op) {
    removed.write_op->vt->on_abort(removed.write_op->block, error::operation_aborted);
  }

  wakeup();
}

inline void io_context_impl::add_work_guard() noexcept {
  work_guard_counter_.fetch_add(1, std::memory_order_acq_rel);
}

inline void io_context_impl::remove_work_guard() noexcept {
  auto const old = work_guard_counter_.fetch_sub(1, std::memory_order_acq_rel);
  IOCORO_ENSURE(old > 0,
                "io_context_impl: remove_work_guard() called more times than add_work_guard()");

  if (old == 1) {
    wakeup();
  }
}

inline auto io_context_impl::process_timers() -> std::size_t {
  std::unique_lock lk{timer_mutex_};
  auto const now = std::chrono::steady_clock::now();
  std::size_t count = 0;

  while (!timers_.empty()) {
    if (stopped_.load(std::memory_order_acquire)) {
      break;
    }

    auto entry = timers_.top();

    // Lazy cancellation: skip cancelled timers and call on_abort
    if (entry->is_cancelled()) {
      timers_.pop();
      auto op = std::move(entry->op);
      lk.unlock();
      if (op) {
        op->vt->on_abort(op->block, error::operation_aborted);
      }
      lk.lock();
      continue;
    }

    // Check if expired
    if (entry->expiry > now) {
      break;
    }

    // Remove from heap before executing
    timers_.pop();

    // Attempt to mark as fired (may lose to concurrent cancellation)
    if (!entry->mark_fired()) {
      continue;
    }

    // Successfully fired, call on_ready
    auto op = std::move(entry->op);
    lk.unlock();
    if (op) {
      op->vt->on_complete(op->block);
    }

    ++count;
    lk.lock();
  }

  return count;
}

inline auto io_context_impl::process_posted() -> std::size_t {
  std::queue<unique_function<void()>> local;
  {
    std::scoped_lock lk{posted_mutex_};
    std::swap(local, posted_operations_);
  }

  std::size_t n = 0;

  if (local.empty()) {
    return 0;
  }

  while (!local.empty()) {
    if (stopped_.load(std::memory_order_acquire)) {
      // Preserve remaining work so a later restart() can still run it.
      std::scoped_lock lk{posted_mutex_};
      while (!local.empty()) {
        posted_operations_.push(std::move(local.front()));
        local.pop();
      }
      break;
    }

    auto f = std::move(local.front());
    local.pop();
    if (f) {
      f();
    }
    ++n;
  }
  return n;
}

inline auto io_context_impl::get_timeout() -> std::optional<std::chrono::milliseconds> {
  std::scoped_lock lk{timer_mutex_};

  if (timers_.empty()) {
    return std::nullopt;
  }

  if (timers_.top()->is_cancelled()) {
    return std::chrono::milliseconds(0);
  }

  auto const now = std::chrono::steady_clock::now();
  auto const expiry = timers_.top()->expiry;
  if (expiry <= now) {
    return std::chrono::milliseconds(0);
  }
  return std::chrono::duration_cast<std::chrono::milliseconds>(expiry - now);
}

inline auto io_context_impl::has_work() -> bool {
  if (work_guard_counter_.load(std::memory_order_acquire) > 0) {
    return true;
  }

  {
    std::scoped_lock lk{fd_mutex_};
    if (!fd_operations_.empty()) {
      return true;
    }
  }

  {
    std::scoped_lock lk{timer_mutex_};
    if (!timers_.empty()) {
      return true;
    }
  }

  {
    std::scoped_lock lk{posted_mutex_};
    if (!posted_operations_.empty()) {
      return true;
    }
  }

  return false;
}

inline void io_context_impl::reconcile_fd_interest(int fd) {
  bool want_read = false;
  bool want_write = false;
  {
    std::scoped_lock lk{fd_mutex_};
    auto it = fd_operations_.find(fd);
    if (it != fd_operations_.end()) {
      want_read = (it->second.read_op != nullptr);
      want_write = (it->second.write_op != nullptr);
    }
  }

  if (want_read || want_write) {
    backend_update_fd_interest(fd, want_read, want_write);
  } else {
    backend_remove_fd_interest(fd);
  }
}

inline void io_context_impl::cancel_fd_event(int fd, detail::fd_event_kind kind,
                                             std::uint64_t token) noexcept {
  reactor_op_ptr removed;
  bool matched = false;

  {
    std::scoped_lock lk{fd_mutex_};
    auto it = fd_operations_.find(fd);
    if (it == fd_operations_.end()) {
      return;
    }

    auto& ops = it->second;

    if (kind == detail::fd_event_kind::read) {
      if (ops.read_op && ops.read_token == token) {
        removed = std::move(ops.read_op);
        ops.read_token = 0;
        matched = true;
      }
    } else {
      if (ops.write_op && ops.write_token == token) {
        removed = std::move(ops.write_op);
        ops.write_token = 0;
        matched = true;
      }
    }

    if (!matched) {
      return;  // overwritten / already completed / different registration
    }

    if (!ops.read_op && !ops.write_op) {
      fd_operations_.erase(it);
    }
  }

  dispatch([this, fd] { reconcile_fd_interest(fd); });

  if (removed) {
    removed->vt->on_abort(removed->block, error::operation_aborted);
  }
  wakeup();
}

inline void fd_event_handle::cancel() const noexcept {
  if (!valid()) {
    return;
  }

  impl->cancel_fd_event(fd, kind, token);
}

inline void timer_event_handle::cancel() const noexcept {
  if (!valid()) {
    return;
  }

  // Mark the timer as cancelled (lazy cancellation)
  if (entry->cancel()) {
    // Successfully cancelled, wake up the event loop so process_timers can clean it up
    impl->wakeup();
  }
}

}  // namespace iocoro::detail

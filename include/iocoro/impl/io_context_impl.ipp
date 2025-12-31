#include <iocoro/assert.hpp>
#include <iocoro/detail/io_context_impl.hpp>
#include <iocoro/detail/operation_base.hpp>
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

inline auto io_context_impl::schedule_timer(std::chrono::milliseconds timeout,
                                            unique_function<void()> callback)
  -> std::shared_ptr<timer_entry> {
  IOCORO_ASSERT(timeout.count() >= 0, "io_context_impl: negative schedule_timer timeout");
  if (timeout.count() < 0) {
    timeout = std::chrono::milliseconds{0};
  }

  auto entry = std::make_shared<detail::timer_entry>();
  entry->expiry = std::chrono::steady_clock::now() + timeout;
  entry->callback = std::move(callback);
  entry->state.store(timer_state::pending, std::memory_order_release);

  {
    std::scoped_lock lk{timer_mutex_};
    entry->id = next_timer_id_++;
    timers_.push(entry);
  }

  wakeup();
  return entry;
}

inline auto io_context_impl::register_fd_read(int fd, std::unique_ptr<operation_base> op)
  -> fd_event_handle {
  std::unique_ptr<operation_base> old;
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
    old->on_abort(error::operation_aborted);
  }

  reconcile_fd_interest_async(fd);

  wakeup();
  return fd_event_handle{this, fd, fd_event_kind::read, token};
}

inline auto io_context_impl::register_fd_write(int fd, std::unique_ptr<operation_base> op)
  -> fd_event_handle {
  std::unique_ptr<operation_base> old;
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
    old->on_abort(error::operation_aborted);
  }

  reconcile_fd_interest_async(fd);

  wakeup();
  return fd_event_handle{this, fd, fd_event_kind::write, token};
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
  reconcile_fd_interest_async(fd);

  if (removed.read_op) {
    removed.read_op->on_abort(error::operation_aborted);
  }
  if (removed.write_op) {
    removed.write_op->on_abort(error::operation_aborted);
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

    if (entry->is_cancelled()) {
      timers_.pop();
      continue;
    }

    if (entry->expiry > now) {
      break;
    }

    // Remove it from the heap before executing.
    timers_.pop();

    // Attempt to claim it as fired (may lose to cancellation).
    if (!entry->mark_fired()) {
      continue;
    }

    lk.unlock();

    auto cb = std::move(entry->callback);
    if (cb) {
      cb();
    }
    try {
      entry->notify_completion();
    } catch (...) {
      // Best-effort: avoid exceptions escaping the event loop.
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

  while (!timers_.empty() && timers_.top()->is_cancelled()) {
    timers_.pop();
  }

  if (timers_.empty()) {
    return std::nullopt;
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
    while (!timers_.empty() && timers_.top()->is_cancelled()) {
      timers_.pop();
    }
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

inline void io_context_impl::reconcile_fd_interest_async(int fd) {
  // If we're on the context thread, update immediately so the backend state is
  // consistent before we go back to waiting for events.
  if (running_in_this_thread()) {
    reconcile_fd_interest(fd);
    return;
  }

  post([this, fd] { reconcile_fd_interest(fd); });
}

inline void io_context_impl::cancel_fd_event(int fd, fd_event_kind kind,
                                             std::uint64_t token) noexcept {
  std::unique_ptr<operation_base> removed;
  bool matched = false;

  {
    std::scoped_lock lk{fd_mutex_};
    auto it = fd_operations_.find(fd);
    if (it == fd_operations_.end()) {
      return;
    }

    auto& ops = it->second;

    if (kind == fd_event_kind::read) {
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

  reconcile_fd_interest_async(fd);

  if (removed) {
    removed->on_abort(error::operation_aborted);
  }
  wakeup();
}

inline void io_context_impl::fd_event_handle::cancel() const noexcept {
  if (!valid()) {
    return;
  }

  auto* p = impl;
  auto const local_fd = fd;
  auto const local_kind = kind;
  auto const local_token = token;

  if (p->running_in_this_thread()) {
    p->cancel_fd_event(local_fd, local_kind, local_token);
  } else {
    p->post([p, local_fd, local_kind, local_token] {
      p->cancel_fd_event(local_fd, local_kind, local_token);
    });
  }
}

}  // namespace iocoro::detail

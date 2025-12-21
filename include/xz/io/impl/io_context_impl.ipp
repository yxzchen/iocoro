#include <xz/io/detail/context/io_context_impl.hpp>
#include <xz/io/detail/executor/executor_guard.hpp>
#include <xz/io/detail/operation/operation_base.hpp>
#include <xz/io/error.hpp>

#include <algorithm>
#include <chrono>
#include <queue>
#include <system_error>
#include <thread>
#include <utility>

namespace xz::io::detail {

// Backend note:
// - For epoll builds, ctor/dtor + fd registration + process_events/wakeup are implemented in
//   `impl/backends/epoll.ipp`.
// - For uring (or no-backend-yet) builds, we keep a minimal fallback here.
#ifdef IOCORO_HAS_URING
io_context_impl::io_context_impl() = default;

io_context_impl::~io_context_impl() { stop(); }
#endif

auto io_context_impl::run() -> std::size_t {
  set_thread_id();

  std::size_t count = 0;

  while (!stopped_.load(std::memory_order_acquire) && has_work()) {
    count += process_posted();
    count += process_timers();

    if (stopped_.load(std::memory_order_acquire) || !has_work()) {
      break;
    }

    auto const timeout = get_timeout();
    auto const wait = (timeout.count() < 0) ? std::nullopt : std::optional{timeout};
    count += process_events(wait);
  }

  return count;
}

auto io_context_impl::run_one() -> std::size_t {
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

  auto const timeout = get_timeout();
  auto const wait = (timeout.count() < 0) ? std::nullopt : std::optional{timeout};
  return process_events(wait);
}

auto io_context_impl::run_for(std::chrono::milliseconds timeout) -> std::size_t {
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

    auto const timer_timeout = get_timeout();
    std::chrono::milliseconds wait_ms = remaining;
    if (timer_timeout.count() >= 0) {
      wait_ms = std::min(wait_ms, timer_timeout);
    }

    count += process_events(wait_ms);
  }

  return count;
}

void io_context_impl::stop() {
  stopped_.store(true, std::memory_order_release);
  wakeup();
}

void io_context_impl::restart() { stopped_.store(false, std::memory_order_release); }

void io_context_impl::post(std::function<void()> f) {
  {
    std::scoped_lock lk{posted_mutex_};
    posted_operations_.push(std::move(f));
  }
  wakeup();
}

void io_context_impl::dispatch(std::function<void()> f) {
  if (running_in_this_thread()) {
    // Ensure awaitables that inherit the "current executor" see the right executor.
    auto const desired = executor{*this};
    if (get_current_executor() == desired) {
      f();
    } else {
      executor_guard g{desired};
      f();
    }
  } else {
    post(std::move(f));
  }
}

auto io_context_impl::schedule_timer(std::chrono::milliseconds timeout,
                                     std::function<void()> callback)
  -> std::shared_ptr<timer_entry> {
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

#ifdef IOCORO_HAS_URING
void io_context_impl::register_fd_read(int fd, std::unique_ptr<operation_base> op) {
  std::unique_ptr<operation_base> old;
  {
    std::scoped_lock lk{fd_mutex_};
    auto& ops = fd_operations_[fd];
    old = std::exchange(ops.read_op, std::move(op));
  }
  if (old) {
    old->abort(make_error_code(error::operation_aborted));
  }
  wakeup();
}

void io_context_impl::register_fd_write(int fd, std::unique_ptr<operation_base> op) {
  std::unique_ptr<operation_base> old;
  {
    std::scoped_lock lk{fd_mutex_};
    auto& ops = fd_operations_[fd];
    old = std::exchange(ops.write_op, std::move(op));
  }
  if (old) {
    old->abort(make_error_code(error::operation_aborted));
  }
  wakeup();
}

void io_context_impl::deregister_fd(int fd) {
  fd_ops removed;
  {
    std::scoped_lock lk{fd_mutex_};
    auto it = fd_operations_.find(fd);
    if (it == fd_operations_.end()) {
      return;
    }
    removed = std::move(it->second);
    fd_operations_.erase(it);
  }

  auto const ec = make_error_code(error::operation_aborted);
  if (removed.read_op) {
    removed.read_op->abort(ec);
  }
  if (removed.write_op) {
    removed.write_op->abort(ec);
  }

  wakeup();
}
#endif

void io_context_impl::add_work_guard() noexcept {
  work_guard_counter_.fetch_add(1, std::memory_order_acq_rel);
}

void io_context_impl::remove_work_guard() noexcept {
  auto const old = work_guard_counter_.fetch_sub(1, std::memory_order_acq_rel);
  if (old == 1) {
    wakeup();
  }
}

void io_context_impl::set_thread_id() noexcept {
  thread_id_.store(std::this_thread::get_id(), std::memory_order_release);
}

auto io_context_impl::running_in_this_thread() const noexcept -> bool {
  return thread_id_.load(std::memory_order_acquire) == std::this_thread::get_id();
}

#ifdef IOCORO_HAS_URING
auto io_context_impl::process_events(std::optional<std::chrono::milliseconds> max_wait)
  -> std::size_t {
  // Platform-independent fallback:
  // - backends (epoll/uring/...) should replace this with a reactor-driven implementation
  //   that completes fd operations and integrates wakeups.
  // - For now, we only provide waiting semantics to avoid a tight busy-loop.
  if (max_wait.has_value()) {
    if (max_wait->count() > 0) {
      std::this_thread::sleep_for(*max_wait);
    }
  } else {
    // "Infinite" wait: yield so we don't spin if nothing else can wake us up.
    std::this_thread::yield();
  }
  return 0;
}
#endif

auto io_context_impl::process_timers() -> std::size_t {
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

    auto cb = std::move(entry->callback);
    lk.unlock();
    {
      if (cb) {
        executor_guard g{executor{*this}};
        cb();
      }
    }
    ++count;
    lk.lock();
  }

  return count;
}

auto io_context_impl::process_posted() -> std::size_t {
  std::queue<std::function<void()>> local;
  {
    std::scoped_lock lk{posted_mutex_};
    std::swap(local, posted_operations_);
  }

  std::size_t n = 0;

  if (local.empty()) {
    return 0;
  }

  executor_guard g{executor{*this}};

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

auto io_context_impl::get_timeout() -> std::chrono::milliseconds {
  std::scoped_lock lk{timer_mutex_};

  while (!timers_.empty() && timers_.top()->is_cancelled()) {
    timers_.pop();
  }

  if (timers_.empty()) {
    return std::chrono::milliseconds(-1);
  }

  auto const now = std::chrono::steady_clock::now();
  auto const expiry = timers_.top()->expiry;
  if (expiry <= now) {
    return std::chrono::milliseconds(0);
  }
  return std::chrono::duration_cast<std::chrono::milliseconds>(expiry - now);
}

#ifdef IOCORO_HAS_URING
void io_context_impl::wakeup() {
  // Platform-independent fallback: no-op.
  // Backends should signal their reactor wait primitive here (eventfd, io_uring_enter, ...).
}
#endif

auto io_context_impl::has_work() -> bool {
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

}  // namespace xz::io::detail

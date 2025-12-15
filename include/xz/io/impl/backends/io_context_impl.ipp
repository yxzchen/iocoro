// Common implementation shared by all io_context backends

#include <xz/io/detail/io_context_impl.hpp>
#include <xz/io/detail/current_executor.hpp>

namespace xz::io::detail {

auto io_context_impl::run() -> std::size_t {
  owner_thread_.store(std::this_thread::get_id(), std::memory_order_release);
  std::size_t count = 0;
  while (!stopped_.load(std::memory_order_acquire) && has_work()) {
    count += process_events();
  }
  return count;
}

auto io_context_impl::run_one() -> std::size_t {
  owner_thread_.store(std::this_thread::get_id(), std::memory_order_release);
  return process_events();
}

auto io_context_impl::run_for(std::chrono::milliseconds timeout) -> std::size_t {
  owner_thread_.store(std::this_thread::get_id(), std::memory_order_release);
  auto const deadline = std::chrono::steady_clock::now() + timeout;
  std::size_t count = 0;
  while (!stopped_.load(std::memory_order_acquire) && has_work()) {
    auto const now = std::chrono::steady_clock::now();
    if (now >= deadline) break;
    auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
    count += process_events(remaining);
  }
  return count;
}

void io_context_impl::stop() {
  stopped_.store(true, std::memory_order_release);
  wakeup();
}

void io_context_impl::restart() {
  stopped_.store(false, std::memory_order_release);
}

void io_context_impl::post(std::function<void()> f) {
  {
    std::lock_guard lock(posted_mutex_);
    posted_operations_.push(std::move(f));
  }
  wakeup();
}

void io_context_impl::dispatch(std::function<void()> f) {
  if (owner_thread_.load(std::memory_order_acquire) == std::this_thread::get_id()) {
    executor_guard g(*owner_);
    f();
  } else {
    post(std::move(f));
  }
}

auto io_context_impl::schedule_timer(std::chrono::milliseconds timeout, std::function<void()> callback)
    -> timer_handle {
  std::lock_guard lock(timer_mutex_);
  auto const id = next_timer_id_++;
  auto const expiry = std::chrono::steady_clock::now() + timeout;
  auto handle = std::make_shared<timer_entry>(id, expiry, std::move(callback), false);
  timers_.push(handle);
  wakeup();
  return handle;
}

void io_context_impl::cancel_timer(timer_handle handle) {
  if (!handle) return;
  handle->cancelled.store(true, std::memory_order_release);
}

auto io_context_impl::process_timers() -> std::size_t {
  std::unique_lock lock(timer_mutex_);
  auto const now = std::chrono::steady_clock::now();
  std::size_t count = 0;

  while (!timers_.empty()) {
    auto handle = timers_.top();

    if (handle->cancelled.load(std::memory_order_acquire)) {
      timers_.pop();
      continue;
    }

    if (handle->expiry > now) {
      break;
    }

    timers_.pop();

    lock.unlock();
    executor_guard g(*owner_);
    handle->callback();
    ++count;
    lock.lock();
  }

  return count;
}

auto io_context_impl::process_posted() -> std::size_t {
  std::queue<std::function<void()>> ops;
  {
    std::lock_guard lock(posted_mutex_);
    ops.swap(posted_operations_);
  }

  std::size_t count = 0;
  while (!ops.empty()) {
    if (stopped_.load(std::memory_order_acquire)) {
      // Put remaining operations back if stopped
      std::lock_guard lock(posted_mutex_);
      while (!ops.empty()) {
        posted_operations_.push(std::move(ops.front()));
        ops.pop();
      }
      break;
    }
    executor_guard g(*owner_);
    ops.front()();
    ops.pop();
    ++count;
  }

  return count;
}

auto io_context_impl::get_timeout() -> std::chrono::milliseconds {
  std::lock_guard lock(timer_mutex_);

  while (!timers_.empty()) {
    auto handle = timers_.top();

    if (handle->cancelled.load(std::memory_order_acquire)) {
      timers_.pop();
      continue;
    }

    auto const now = std::chrono::steady_clock::now();
    if (handle->expiry <= now) {
      return std::chrono::milliseconds(0);
    }

    return std::chrono::duration_cast<std::chrono::milliseconds>(handle->expiry - now);
  }

  return std::chrono::milliseconds(-1);
}

auto io_context_impl::has_work() -> bool {
  if (work_guard_counter_.load(std::memory_order_acquire) > 0) {
    return true;
  }

  {
    std::lock_guard lock(fd_mutex_);
    if (!fd_operations_.empty()) {
      return true;
    }
  }

  {
    std::lock_guard lock(timer_mutex_);
    // Clean up cancelled timers
    while (!timers_.empty() && timers_.top()->cancelled.load(std::memory_order_acquire)) {
      timers_.pop();
    }
    if (!timers_.empty()) {
      return true;
    }
  }

  {
    std::lock_guard lock(posted_mutex_);
    if (!posted_operations_.empty()) {
      return true;
    }
  }

  return false;
}

void io_context_impl::add_work_guard() noexcept {
  work_guard_counter_.fetch_add(1, std::memory_order_acq_rel);
}

void io_context_impl::remove_work_guard() noexcept {
  auto old_value = work_guard_counter_.fetch_sub(1, std::memory_order_acq_rel);

  if (old_value == 1) {
    wakeup();
  }
}

}  // namespace xz::io::detail

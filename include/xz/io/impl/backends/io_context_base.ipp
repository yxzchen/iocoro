// Common implementation shared by all io_context backends

#include <xz/io/detail/io_context_impl.hpp>

namespace xz::io::detail {

// Run event loop until stopped
auto io_context_impl::run() -> std::size_t {
  stopped_.store(false, std::memory_order_release);
  owner_thread_.store(std::this_thread::get_id(), std::memory_order_release);
  std::size_t count = 0;
  while (!stopped_.load(std::memory_order_acquire)) {
    count += run_one();
  }
  return count;
}

// Run one iteration of event loop
auto io_context_impl::run_one() -> std::size_t {
  auto timeout = get_timeout();
  return process_events(timeout);
}

// Run event loop for specified duration
auto io_context_impl::run_for(std::chrono::milliseconds timeout) -> std::size_t {
  stopped_.store(false, std::memory_order_release);
  owner_thread_.store(std::this_thread::get_id(), std::memory_order_release);
  auto const deadline = std::chrono::steady_clock::now() + timeout;
  std::size_t count = 0;
  while (!stopped_.load(std::memory_order_acquire)) {
    auto const now = std::chrono::steady_clock::now();
    if (now >= deadline) break;
    auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
    count += process_events(remaining);
  }
  return count;
}

// Stop the event loop
void io_context_impl::stop() {
  stopped_.store(true, std::memory_order_release);
  wakeup();
}

// Restart the event loop
void io_context_impl::restart() {
  stopped_.store(false, std::memory_order_release);
}

// Post work to be executed by the event loop
void io_context_impl::post(std::function<void()> f) {
  {
    std::lock_guard lock(posted_mutex_);
    posted_operations_.push(std::move(f));
  }
  wakeup();
}

// Dispatch work (execute immediately if on event loop thread, otherwise post)
void io_context_impl::dispatch(std::function<void()> f) {
  if (owner_thread_.load(std::memory_order_acquire) == std::this_thread::get_id()) {
    f();
  } else {
    post(std::move(f));
  }
}

// Schedule a timer
auto io_context_impl::schedule_timer(std::chrono::milliseconds timeout, std::function<void()> callback)
    -> timer_handle {
  std::lock_guard lock(timer_mutex_);
  auto const id = next_timer_id_++;
  auto const expiry = std::chrono::steady_clock::now() + timeout;
  auto handle = std::shared_ptr<timer_entry>(new timer_entry{id, expiry, std::move(callback), false});
  timers_.push(handle);
  wakeup();
  return handle;
}

// Cancel a timer
void io_context_impl::cancel_timer(timer_handle handle) {
  if (!handle) return;
  handle->cancelled.store(true, std::memory_order_release);
}

// Process expired timers
void io_context_impl::process_timers() {
  std::lock_guard lock(timer_mutex_);
  auto const now = std::chrono::steady_clock::now();

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

    auto callback = std::move(handle->callback);
    timer_mutex_.unlock();
    callback();
    timer_mutex_.lock();
  }
}

// Process posted operations
void io_context_impl::process_posted() {
  std::queue<std::function<void()>> ops;
  {
    std::lock_guard lock(posted_mutex_);
    ops.swap(posted_operations_);
  }

  while (!ops.empty()) {
    ops.front()();
    ops.pop();
  }
}

// Get timeout for next event wait
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

}  // namespace xz::io::detail

#include <xz/io/detail/context/io_context_impl.hpp>

#include <cerrno>
#include <stdexcept>

namespace xz::io::detail {

io_context_impl::io_context_impl() {}

io_context_impl::~io_context_impl() { stop(); }

void io_context_impl::post(std::function<void()> f) {
  {
    std::scoped_lock lk{posted_mutex_};
    posted_operations_.push(std::move(f));
  }
  wakeup();
}

void io_context_impl::dispatch(std::function<void()> f) {
  if (running_in_this_thread()) {
    f();
  } else {
    post(std::move(f));
  }
}

auto io_context_impl::run() -> std::size_t {
  thread_id_.store(std::this_thread::get_id(), std::memory_order_release);
  std::size_t n = 0;
  while (!stopped_.load(std::memory_order_acquire)) {
    n += process_posted();
    if (!has_work()) break;
    // In this minimal implementation, we don't process timers/fd events yet.
    std::this_thread::yield();
  }
  return n;
}

auto io_context_impl::run_one() -> std::size_t {
  thread_id_.store(std::this_thread::get_id(), std::memory_order_release);
  return process_posted();
}

auto io_context_impl::run_for(std::chrono::milliseconds) -> std::size_t {
  // Minimal implementation: just drain posted operations once.
  return run_one();
}

void io_context_impl::stop() { stopped_.store(true, std::memory_order_release); }

void io_context_impl::restart() { stopped_.store(false, std::memory_order_release); }

auto io_context_impl::running_in_this_thread() const noexcept -> bool {
  return thread_id_.load(std::memory_order_acquire) == std::this_thread::get_id();
}

auto io_context_impl::schedule_timer(std::chrono::milliseconds, std::function<void()>)
  -> std::shared_ptr<timer_entry> {
  throw std::runtime_error("io_context_impl::schedule_timer not implemented");
}

void io_context_impl::register_fd_read(int, std::unique_ptr<operation_base>) {
  throw std::runtime_error("io_context_impl::register_fd_read not implemented");
}

void io_context_impl::register_fd_write(int, std::unique_ptr<operation_base>) {
  throw std::runtime_error("io_context_impl::register_fd_write not implemented");
}

void io_context_impl::deregister_fd(int) {
  throw std::runtime_error("io_context_impl::deregister_fd not implemented");
}

void io_context_impl::add_work_guard() noexcept {
  work_guard_counter_.fetch_add(1, std::memory_order_acq_rel);
}

void io_context_impl::remove_work_guard() noexcept {
  work_guard_counter_.fetch_sub(1, std::memory_order_acq_rel);
}

auto io_context_impl::process_events(std::optional<std::chrono::milliseconds>) -> std::size_t {
  return 0;
}

auto io_context_impl::process_timers() -> std::size_t { return 0; }

auto io_context_impl::process_posted() -> std::size_t {
  std::queue<std::function<void()>> local;
  {
    std::scoped_lock lk{posted_mutex_};
    std::swap(local, posted_operations_);
  }

  std::size_t n = 0;
  while (!local.empty()) {
    auto f = std::move(local.front());
    local.pop();
    if (f) f();
    ++n;
  }
  return n;
}

auto io_context_impl::get_timeout() -> std::chrono::milliseconds {
  return std::chrono::milliseconds{0};
}

void io_context_impl::wakeup() {
  // Minimal: no-op (no eventfd/epoll integration yet).
}

auto io_context_impl::has_work() -> bool {
  if (work_guard_counter_.load(std::memory_order_acquire) > 0) return true;
  std::scoped_lock lk{posted_mutex_};
  return !posted_operations_.empty();
}

}  // namespace xz::io::detail

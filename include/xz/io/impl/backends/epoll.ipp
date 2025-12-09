
#include <xz/io/detail/io_context_impl.hpp>

#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <cerrno>

namespace xz::io::detail {

io_context_impl::io_context_impl() {
  epoll_fd_ = ::epoll_create1(EPOLL_CLOEXEC);
  if (epoll_fd_ < 0) {
    throw std::system_error(errno, std::generic_category(), "epoll_create1 failed");
  }

  eventfd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (eventfd_ < 0) {
    ::close(epoll_fd_);
    throw std::system_error(errno, std::generic_category(), "eventfd failed");
  }

  epoll_event ev{
      .events = EPOLLIN | EPOLLET,
      .data = {.fd = eventfd_}};
  if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, eventfd_, &ev) < 0) {
    ::close(eventfd_);
    ::close(epoll_fd_);
    throw std::system_error(errno, std::generic_category(), "epoll_ctl failed");
  }

  owner_thread_.store({}, std::memory_order_release);
}

io_context_impl::~io_context_impl() {
  if (eventfd_ >= 0) ::close(eventfd_);
  if (epoll_fd_ >= 0) ::close(epoll_fd_);
}

auto io_context_impl::run() -> std::size_t {
  stopped_.store(false, std::memory_order_release);
  owner_thread_.store(std::this_thread::get_id(), std::memory_order_release);

  std::size_t count = 0;
  while (!stopped_.load(std::memory_order_acquire)) {
    count += run_one();
  }

  return count;
}

auto io_context_impl::run_one() -> std::size_t {
  auto timeout = get_timeout();
  return process_events(timeout);
}

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
    f();
  } else {
    post(std::move(f));
  }
}

void io_context_impl::register_fd_read(int fd, std::unique_ptr<io_context::operation_base> op) {
  std::lock_guard lock(fd_mutex_);

  epoll_event ev{
      .events = EPOLLIN | EPOLLET,
      .data = {.fd = fd}};

  auto& ops = fd_operations_[fd];
  ops.read_op = std::move(op);

  int res = ::epoll_ctl(epoll_fd_, ops.write_op ? EPOLL_CTL_MOD : EPOLL_CTL_ADD, fd, &ev);
  if (res < 0 && errno != EEXIST) {
    fd_operations_.erase(fd);
    throw std::system_error(errno, std::generic_category(), "epoll_ctl failed");
  }
}

void io_context_impl::register_fd_write(int fd, std::unique_ptr<io_context::operation_base> op) {
  std::lock_guard lock(fd_mutex_);

  epoll_event ev{
      .events = EPOLLOUT | EPOLLET,
      .data = {.fd = fd}};

  auto& ops = fd_operations_[fd];
  ops.write_op = std::move(op);

  int res = ::epoll_ctl(epoll_fd_, ops.read_op ? EPOLL_CTL_MOD : EPOLL_CTL_ADD, fd, &ev);
  if (res < 0 && errno != EEXIST) {
    fd_operations_.erase(fd);
    throw std::system_error(errno, std::generic_category(), "epoll_ctl failed");
  }
}

void io_context_impl::register_fd_readwrite(int fd, std::unique_ptr<io_context::operation_base> read_op,
                                            std::unique_ptr<io_context::operation_base> write_op) {
  std::lock_guard lock(fd_mutex_);

  epoll_event ev{
      .events = EPOLLIN | EPOLLOUT | EPOLLET,
      .data = {.fd = fd}};

  auto& ops = fd_operations_[fd];
  ops.read_op = std::move(read_op);
  ops.write_op = std::move(write_op);

  int res = ::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev);
  if (res < 0 && errno != EEXIST) {
    res = ::epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &ev);
    if (res < 0) {
      fd_operations_.erase(fd);
      throw std::system_error(errno, std::generic_category(), "epoll_ctl failed");
    }
  }
}

void io_context_impl::deregister_fd(int fd) {
  std::lock_guard lock(fd_mutex_);

  ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
  fd_operations_.erase(fd);
}

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

void io_context_impl::cancel_timer(timer_handle handle) {
  if (!handle) return;
  handle->cancelled.store(true, std::memory_order_release);
}

auto io_context_impl::process_events(std::chrono::milliseconds timeout) -> std::size_t {
  constexpr int max_events = 64;
  epoll_event events[max_events];

  int timeout_ms = timeout.count() < 0 ? -1 : static_cast<int>(timeout.count());
  int nfds = ::epoll_wait(epoll_fd_, events, max_events, timeout_ms);

  if (nfds < 0) {
    if (errno == EINTR) return 0;
    throw std::system_error(errno, std::generic_category(), "epoll_wait failed");
  }

  std::size_t count = 0;

  process_timers();
  process_posted();

  for (int i = 0; i < nfds; ++i) {
    int fd = events[i].data.fd;
    uint32_t ev = events[i].events;

    if (fd == eventfd_) {
      uint64_t value;
      [[maybe_unused]] auto _ = ::read(eventfd_, &value, sizeof(value));
      continue;
    }

    std::unique_ptr<io_context::operation_base> read_op;
    std::unique_ptr<io_context::operation_base> write_op;

    {
      std::lock_guard lock(fd_mutex_);
      auto it = fd_operations_.find(fd);
      if (it != fd_operations_.end()) {
        if (ev & EPOLLIN) read_op = std::move(it->second.read_op);
        if (ev & EPOLLOUT) write_op = std::move(it->second.write_op);

        if (!it->second.read_op && !it->second.write_op) {
          fd_operations_.erase(it);
        }
      }
    }

    if (read_op) {
      read_op->execute();
      ++count;
    }
    if (write_op) {
      write_op->execute();
      ++count;
    }
  }

  return count;
}

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

void io_context_impl::wakeup() {
  uint64_t value = 1;
  [[maybe_unused]] auto _ = ::write(eventfd_, &value, sizeof(value));
}

}  // namespace xz::io::detail


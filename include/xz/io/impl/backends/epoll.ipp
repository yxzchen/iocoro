
#include <xz/io/impl/backends/io_context_impl.ipp>
#include <xz/io/detail/operation_base.hpp>

#include <cerrno>

#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

namespace xz::io::detail {

io_context_impl::io_context_impl(io_context* owner) : owner_(owner) {
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

void io_context_impl::register_fd_read(int fd, std::unique_ptr<operation_base> op) {
  std::lock_guard lock(fd_mutex_);

  auto& ops = fd_operations_[fd];
  ops.read_op = std::move(op);

  epoll_event ev{
      .events = EPOLLIN | (ops.write_op ? EPOLLOUT : 0u) | EPOLLET,
      .data = {.fd = fd}};

  int res = ::epoll_ctl(epoll_fd_, ops.write_op ? EPOLL_CTL_MOD : EPOLL_CTL_ADD, fd, &ev);
  if (res < 0 && errno != EEXIST) {
    fd_operations_.erase(fd);
    throw std::system_error(errno, std::generic_category(), "epoll_ctl failed");
  }
}

void io_context_impl::register_fd_write(int fd, std::unique_ptr<operation_base> op) {
  std::lock_guard lock(fd_mutex_);

  auto& ops = fd_operations_[fd];
  ops.write_op = std::move(op);

  epoll_event ev{
      .events = EPOLLOUT | (ops.read_op ? EPOLLIN : 0u) | EPOLLET,
      .data = {.fd = fd}};

  int res = ::epoll_ctl(epoll_fd_, ops.read_op ? EPOLL_CTL_MOD : EPOLL_CTL_ADD, fd, &ev);
  if (res < 0 && errno != EEXIST) {
    fd_operations_.erase(fd);
    throw std::system_error(errno, std::generic_category(), "epoll_ctl failed");
  }
}

void io_context_impl::register_fd_readwrite(int fd, std::unique_ptr<operation_base> read_op,
                                            std::unique_ptr<operation_base> write_op) {
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

auto io_context_impl::process_events(std::optional<std::chrono::milliseconds> max_wait) -> std::size_t {
  executor_guard tick_guard(*owner_);
  std::size_t count = 0;
  count += process_timers();
  count += process_posted();

  if (!has_work()) {
    return count;
  }

  auto timeout = get_timeout();
  if (max_wait.has_value()) {
    timeout = (timeout.count() < 0 ? *max_wait : std::min(timeout, *max_wait));
  }
  
  constexpr int max_events = 64;
  epoll_event events[max_events];

  int timeout_ms = timeout.count() < 0 ? -1 : static_cast<int>(timeout.count());
  int nfds = ::epoll_wait(epoll_fd_, events, max_events, timeout_ms);

  if (nfds < 0) {
    if (errno == EINTR) return count;
    throw std::system_error(errno, std::generic_category(), "epoll_wait failed");
  }

  for (int i = 0; i < nfds; ++i) {
    int fd = events[i].data.fd;
    uint32_t ev = events[i].events;

    if (fd == eventfd_) {
      uint64_t value;
      while (::read(eventfd_, &value, sizeof(value)) > 0);
      continue;
    }

    std::unique_ptr<operation_base> read_op;
    std::unique_ptr<operation_base> write_op;

    // Check for error conditions first
    bool is_error = ev & (EPOLLERR | EPOLLHUP | EPOLLRDHUP);

    {
      std::lock_guard lock(fd_mutex_);
      auto it = fd_operations_.find(fd);
      if (it != fd_operations_.end()) {
        if (is_error || (ev & EPOLLIN)) read_op = std::move(it->second.read_op);
        if (is_error || (ev & EPOLLOUT)) write_op = std::move(it->second.write_op);

        if (!it->second.read_op && !it->second.write_op) {
          ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
          fd_operations_.erase(it);
        }
      }
    }

    if (is_error) {
      if (read_op) {
        read_op->abort(std::make_error_code(std::errc::connection_reset));
        ++count;
      }
      if (write_op) {
        write_op->abort(std::make_error_code(std::errc::connection_reset));
        ++count;
      }
    } else {
      if (read_op) {
        read_op->execute();
        ++count;
      }
      if (write_op) {
        write_op->execute();
        ++count;
      }
    }
  }

  return count;
}

void io_context_impl::wakeup() {
  uint64_t value = 1;
  ssize_t n;
  do {
      n = ::write(eventfd_, &value, sizeof(value));
  } while (n < 0 && errno == EINTR);
}

}  // namespace xz::io::detail

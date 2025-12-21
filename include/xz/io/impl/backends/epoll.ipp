#include <xz/io/detail/context/io_context_impl.hpp>
#include <xz/io/detail/executor/executor_guard.hpp>
#include <xz/io/detail/operation/operation_base.hpp>
#include <xz/io/error.hpp>

#include <cerrno>
#include <cstdint>
#include <system_error>

#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

namespace xz::io::detail {

struct io_context_impl::backend_impl {
  int epoll_fd = -1;
  int eventfd = -1;
};

auto io_context_impl::native_handle() const noexcept -> std::uintptr_t {
  return static_cast<std::uintptr_t>(backend_->epoll_fd);
}

namespace {

void close_if_valid(int& fd) noexcept {
  if (fd >= 0) {
    ::close(fd);
    fd = -1;
  }
}

void drain_eventfd(int eventfd) noexcept {
  std::uint64_t value = 0;
  for (;;) {
    auto const n = ::read(eventfd, &value, sizeof(value));
    if (n > 0) {
      continue;
    }
    if (n < 0 && errno == EINTR) {
      continue;
    }
    break;
  }
}

}  // namespace

io_context_impl::io_context_impl() {
  backend_ = std::make_unique<backend_impl>();

  backend_->epoll_fd = ::epoll_create1(EPOLL_CLOEXEC);
  if (backend_->epoll_fd < 0) {
    throw std::system_error(errno, std::generic_category(), "epoll_create1 failed");
  }

  backend_->eventfd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (backend_->eventfd < 0) {
    close_if_valid(backend_->epoll_fd);
    throw std::system_error(errno, std::generic_category(), "eventfd failed");
  }

  epoll_event ev{};
  ev.events = EPOLLIN | EPOLLET;
  ev.data.fd = backend_->eventfd;
  if (::epoll_ctl(backend_->epoll_fd, EPOLL_CTL_ADD, backend_->eventfd, &ev) < 0) {
    close_if_valid(backend_->eventfd);
    close_if_valid(backend_->epoll_fd);
    throw std::system_error(errno, std::generic_category(), "epoll_ctl(add eventfd) failed");
  }
}

io_context_impl::~io_context_impl() {
  stop();
  if (backend_) {
    close_if_valid(backend_->eventfd);
    close_if_valid(backend_->epoll_fd);
  }
}

void io_context_impl::backend_update_fd_interest(int fd, bool want_read, bool want_write) {
  std::uint32_t events = static_cast<std::uint32_t>(EPOLLET);
  if (want_read) {
    events |= static_cast<std::uint32_t>(EPOLLIN);
  }
  if (want_write) {
    events |= static_cast<std::uint32_t>(EPOLLOUT);
  }

  epoll_event ev{};
  ev.events = events;
  ev.data.fd = fd;

  if (::epoll_ctl(backend_->epoll_fd, EPOLL_CTL_MOD, fd, &ev) == 0) {
    return;
  }

  if (errno == ENOENT) {
    if (::epoll_ctl(backend_->epoll_fd, EPOLL_CTL_ADD, fd, &ev) == 0) {
      return;
    }
  }

  throw std::system_error(errno, std::generic_category(), "epoll_ctl (add/mod) failed");
}

void io_context_impl::backend_remove_fd_interest(int fd) noexcept {
  if (backend_->epoll_fd < 0 || fd < 0) {
    return;
  }
  ::epoll_ctl(backend_->epoll_fd, EPOLL_CTL_DEL, fd, nullptr);
}

auto io_context_impl::process_events(std::optional<std::chrono::milliseconds> max_wait)
  -> std::size_t {
  executor_guard g{executor{*this}};

  int timeout_ms = -1;
  if (max_wait.has_value()) {
    timeout_ms = static_cast<int>(max_wait->count());
  }

  constexpr int max_events = 128;
  epoll_event events[max_events]{};

  int nfds = ::epoll_wait(backend_->epoll_fd, events, max_events, timeout_ms);
  if (nfds < 0) {
    if (errno == EINTR) {
      return 0;
    }
    throw std::system_error(errno, std::generic_category(), "epoll_wait failed");
  }

  std::size_t count = 0;

  for (int i = 0; i < nfds; ++i) {
    int const fd = events[i].data.fd;
    std::uint32_t const ev = events[i].events;

    if (fd == backend_->eventfd) {
      drain_eventfd(backend_->eventfd);
      continue;
    }

    bool const is_error =
      (ev & (static_cast<std::uint32_t>(EPOLLERR) | static_cast<std::uint32_t>(EPOLLHUP) |
             static_cast<std::uint32_t>(EPOLLRDHUP))) != 0;

    std::unique_ptr<operation_base> read_op;
    std::unique_ptr<operation_base> write_op;
    bool still_want_read = false;
    bool still_want_write = false;

    {
      std::scoped_lock lk{fd_mutex_};
      auto it = fd_operations_.find(fd);
      if (it != fd_operations_.end()) {
        if (is_error || ((ev & static_cast<std::uint32_t>(EPOLLIN)) != 0)) {
          read_op = std::move(it->second.read_op);
        }
        if (is_error || ((ev & static_cast<std::uint32_t>(EPOLLOUT)) != 0)) {
          write_op = std::move(it->second.write_op);
        }

        still_want_read = (it->second.read_op != nullptr);
        still_want_write = (it->second.write_op != nullptr);

        if (!still_want_read && !still_want_write) {
          fd_operations_.erase(it);
        }
      }
    }

    if (!still_want_read && !still_want_write) {
      backend_remove_fd_interest(fd);
    } else {
      backend_update_fd_interest(fd, still_want_read, still_want_write);
    }

    if (is_error) {
      auto const ec = std::make_error_code(std::errc::connection_reset);
      if (read_op) {
        read_op->abort(ec);
        ++count;
      }
      if (write_op) {
        write_op->abort(ec);
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
  std::uint64_t value = 1;
  for (;;) {
    auto const n = ::write(backend_->eventfd, &value, sizeof(value));
    if (n >= 0) {
      return;
    }
    if (errno == EINTR) {
      continue;
    }
    // Best-effort wakeup: if the counter is "full" (EAGAIN) we can ignore it.
    return;
  }
}

}  // namespace xz::io::detail

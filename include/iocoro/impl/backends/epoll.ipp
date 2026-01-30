#include <iocoro/detail/reactor_backend.hpp>
#include <iocoro/error.hpp>

#include <cerrno>
#include <cstdint>
#include <system_error>

#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

namespace iocoro::detail {

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

class backend_epoll final : public backend_interface {
 public:
  backend_epoll() {
    epoll_fd_ = ::epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd_ < 0) {
      throw std::system_error(errno, std::generic_category(), "epoll_create1 failed");
    }

    eventfd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (eventfd_ < 0) {
      close_if_valid(epoll_fd_);
      throw std::system_error(errno, std::generic_category(), "eventfd failed");
    }

    epoll_event ev{};
    ev.events = EPOLLIN | EPOLLET;
    ev.data.fd = eventfd_;
    if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, eventfd_, &ev) < 0) {
      close_if_valid(eventfd_);
      close_if_valid(epoll_fd_);
      throw std::system_error(errno, std::generic_category(), "epoll_ctl(add eventfd) failed");
    }
  }

  ~backend_epoll() override {
    close_if_valid(eventfd_);
    close_if_valid(epoll_fd_);
  }

  void update_fd_interest(int fd, bool want_read, bool want_write) override {
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

    if (::epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &ev) == 0) {
      return;
    }

    if (errno == ENOENT) {
      if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev) == 0) {
        return;
      }
    }

    throw std::system_error(errno, std::generic_category(), "epoll_ctl (add/mod) failed");
  }

  void remove_fd_interest(int fd) noexcept override {
    if (epoll_fd_ < 0 || fd < 0) {
      return;
    }
    ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
  }

  auto wait(std::optional<std::chrono::milliseconds> timeout) -> std::vector<backend_event> override {
    int timeout_ms = -1;
    if (timeout.has_value()) {
      timeout_ms = static_cast<int>(timeout->count());
    }

    constexpr int max_events = 128;
    epoll_event events[max_events]{};

    int nfds = ::epoll_wait(epoll_fd_, events, max_events, timeout_ms);
    if (nfds < 0) {
      if (errno == EINTR) {
        return {};
      }
      throw std::system_error(errno, std::generic_category(), "epoll_wait failed");
    }

    std::vector<backend_event> out;
    out.reserve(static_cast<std::size_t>(nfds));

    for (int i = 0; i < nfds; ++i) {
      int const fd = events[i].data.fd;
      std::uint32_t const ev = events[i].events;

      if (fd == eventfd_) {
        drain_eventfd(eventfd_);
        continue;
      }

      bool const is_error =
        (ev & (static_cast<std::uint32_t>(EPOLLERR) | static_cast<std::uint32_t>(EPOLLHUP) |
               static_cast<std::uint32_t>(EPOLLRDHUP))) != 0;

      backend_event e{};
      e.fd = fd;
      e.is_error = is_error;
      e.can_read = is_error || ((ev & static_cast<std::uint32_t>(EPOLLIN)) != 0);
      e.can_write = is_error || ((ev & static_cast<std::uint32_t>(EPOLLOUT)) != 0);

      if (is_error) {
        if (ev & static_cast<std::uint32_t>(EPOLLRDHUP)) {
          e.ec = error::eof;
        } else if (ev & static_cast<std::uint32_t>(EPOLLHUP)) {
          e.ec = error::eof;
        } else {
          e.ec = error::connection_reset;
        }
      }

      out.push_back(e);
    }

    return out;
  }

  void wakeup() noexcept override {
    std::uint64_t value = 1;
    for (;;) {
      auto const n = ::write(eventfd_, &value, sizeof(value));
      if (n >= 0) {
        return;
      }
      if (errno == EINTR) {
        continue;
      }
      return;
    }
  }

 private:
  int epoll_fd_ = -1;
  int eventfd_ = -1;
};

inline auto make_backend() -> std::unique_ptr<backend_interface> {
  return std::make_unique<backend_epoll>();
}

}  // namespace iocoro::detail

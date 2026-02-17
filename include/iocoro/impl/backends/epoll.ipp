#include <iocoro/detail/reactor_backend.hpp>
#include <iocoro/error.hpp>

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <mutex>
#include <system_error>
#include <unordered_map>

#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

namespace iocoro::detail {

namespace {

auto pack_fd_gen(int fd, std::uint32_t gen) noexcept -> std::uint64_t {
  return (static_cast<std::uint64_t>(gen) << 32) |
         static_cast<std::uint64_t>(static_cast<std::uint32_t>(fd));
}

auto unpack_fd(std::uint64_t data) noexcept -> int {
  return static_cast<int>(data & 0xFFFFFFFFULL);
}

auto unpack_gen(std::uint64_t data) noexcept -> std::uint32_t {
  return static_cast<std::uint32_t>(data >> 32);
}

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
    ev.data.u64 = pack_fd_gen(eventfd_, 0);
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

  void add_fd(int fd) override {
    std::uint32_t gen = 1;
    {
      std::scoped_lock lk{fd_states_mtx_};
      auto& st = fd_states_[fd];
      st.generation += 1;
      if (st.generation == 0) {
        st.generation += 1;
      }
      st.active = true;
      gen = st.generation;
    }

    std::uint32_t events =
      static_cast<std::uint32_t>(EPOLLET | EPOLLIN | EPOLLOUT | EPOLLERR | EPOLLHUP | EPOLLRDHUP);

    epoll_event ev{};
    ev.events = events;
    ev.data.u64 = pack_fd_gen(fd, gen);

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

  void remove_fd(int fd) noexcept override {
    if (epoll_fd_ < 0 || fd < 0) {
      return;
    }
    {
      std::scoped_lock lk{fd_states_mtx_};
      auto it = fd_states_.find(fd);
      if (it != fd_states_.end()) {
        it->second.active = false;
      }
    }
    ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
  }

  auto wait(std::optional<std::chrono::milliseconds> timeout,
            std::vector<backend_event>& out) -> void override {
    int timeout_ms = -1;
    if (timeout.has_value()) {
      timeout_ms = static_cast<int>(timeout->count());
    }

    constexpr int max_events = 128;
    epoll_event events[max_events]{};

    int nfds = ::epoll_wait(epoll_fd_, events, max_events, timeout_ms);
    if (nfds < 0) {
      if (errno == EINTR) {
        return;
      }
      throw std::system_error(errno, std::generic_category(), "epoll_wait failed");
    }

    out.clear();
    out.reserve(static_cast<std::size_t>(nfds));

    for (int i = 0; i < nfds; ++i) {
      std::uint64_t const data = events[i].data.u64;
      int const fd = unpack_fd(data);
      std::uint32_t const gen = unpack_gen(data);
      std::uint32_t const ev = events[i].events;

      if (fd == eventfd_) {
        drain_eventfd(eventfd_);
        // Clear the dedupe flag after draining.
        // If a wakeup raced and its token was drained in this batch, we must
        // not leave wakeup_pending_ stuck at true.
        wakeup_pending_.store(false, std::memory_order_release);
        continue;
      }

      {
        std::scoped_lock lk{fd_states_mtx_};
        auto it = fd_states_.find(fd);
        if (it == fd_states_.end() || !it->second.active || it->second.generation != gen) {
          continue;
        }
      }

      bool const has_error = (ev & static_cast<std::uint32_t>(EPOLLERR)) != 0;
      bool const has_hup =
        (ev & (static_cast<std::uint32_t>(EPOLLHUP) | static_cast<std::uint32_t>(EPOLLRDHUP))) != 0;
      bool const has_read = (ev & static_cast<std::uint32_t>(EPOLLIN)) != 0;
      bool const has_write = (ev & static_cast<std::uint32_t>(EPOLLOUT)) != 0;

      backend_event e{};
      e.fd = fd;
      // EPOLLHUP/EPOLLRDHUP may arrive together with unread data. Treat them as readiness so
      // awaiters can perform the syscall and drain buffered bytes before observing EOF.
      e.is_error = has_error;
      e.can_read = has_error || has_hup || has_read;
      e.can_write = has_error || has_hup || has_write;

      if (has_error) {
        e.ec = error::connection_reset;
      }

      out.push_back(e);
    }

    return;
  }

  void wakeup() noexcept override {
    if (wakeup_pending_.exchange(true, std::memory_order_acq_rel)) {
      return;
    }
    std::uint64_t value = 1;
    for (;;) {
      auto const n = ::write(eventfd_, &value, sizeof(value));
      if (n >= 0) {
        return;
      }
      if (errno == EINTR) {
        continue;
      }
      // Best-effort rollback: if write failed, allow future wakeups to retry.
      wakeup_pending_.store(false, std::memory_order_release);
      return;
    }
  }

 private:
  struct epoll_fd_state {
    std::uint32_t generation = 0;
    bool active = false;
  };

  int epoll_fd_ = -1;
  int eventfd_ = -1;
  std::mutex fd_states_mtx_{};
  std::unordered_map<int, epoll_fd_state> fd_states_{};
  std::atomic<bool> wakeup_pending_{false};
};

inline auto make_backend() -> std::unique_ptr<backend_interface> {
  return std::make_unique<backend_epoll>();
}

}  // namespace iocoro::detail

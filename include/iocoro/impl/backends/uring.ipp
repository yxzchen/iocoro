#include <iocoro/detail/reactor_backend.hpp>
#include <iocoro/error.hpp>

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <limits>
#include <mutex>
#include <system_error>
#include <unordered_map>

#include <liburing.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>

namespace iocoro::detail {

namespace {

constexpr std::uint64_t tag_poll = 0;
constexpr std::uint64_t tag_wakeup = 1;
constexpr std::uint64_t tag_remove = 2;

constexpr std::uint64_t fd_shift = 2;
constexpr std::uint64_t gen_shift = 34;

auto pack_fd(int fd, std::uint64_t tag, std::uint32_t gen = 0) noexcept -> std::uint64_t {
  return (static_cast<std::uint64_t>(gen) << gen_shift) |
         (static_cast<std::uint64_t>(static_cast<std::uint32_t>(fd)) << fd_shift) | (tag & 0x3ULL);
}

auto unpack_tag(std::uint64_t data) noexcept -> std::uint64_t {
  return (data & 0x3ULL);
}
auto unpack_fd(std::uint64_t data) noexcept -> int {
  return static_cast<int>((data >> fd_shift) & 0xFFFFFFFFULL);
}
auto unpack_gen(std::uint64_t data) noexcept -> std::uint32_t {
  return static_cast<std::uint32_t>(data >> gen_shift);
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

auto to_timespec(std::chrono::milliseconds ms) noexcept -> __kernel_timespec {
  auto const clamped =
    std::min<long long>(ms.count(), static_cast<long long>(std::numeric_limits<int>::max()));
  __kernel_timespec ts{};
  ts.tv_sec = static_cast<__kernel_time64_t>(clamped / 1000);
  ts.tv_nsec = static_cast<long>(static_cast<long long>(clamped % 1000) * 1000LL * 1000LL);
  return ts;
}

}  // namespace

class backend_uring final : public backend_interface {
 public:
  backend_uring() {
    int const ret = ::io_uring_queue_init(256, &ring_, 0);
    if (ret < 0) {
      throw std::system_error(-ret, std::generic_category(), "io_uring_queue_init failed");
    }

    eventfd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (eventfd_ < 0) {
      ::io_uring_queue_exit(&ring_);
      throw std::system_error(errno, std::generic_category(), "eventfd failed");
    }

    arm_wakeup();
  }

  ~backend_uring() override {
    close_if_valid(eventfd_);
    ::io_uring_queue_exit(&ring_);
  }

  void update_fd_interest(int fd, bool want_read, bool want_write) override {
    int mask = 0;
    if (want_read) {
      mask |= POLLIN;
    }
    if (want_write) {
      mask |= POLLOUT;
    }
    mask |= (POLLERR | POLLHUP | POLLRDHUP);

    {
      std::scoped_lock lk{poll_mtx_};
      auto& st = polls_[fd];
      st.desired_mask = mask;

      if (st.armed) {
        if (st.active_mask == mask) {
          return;
        }
        if (!st.cancel_requested) {
          st.cancel_requested = true;
          pending_removes_.push_back(pending_remove{fd, st.active_user_data});
        }
      } else if (mask != 0) {
        st.armed = true;
        st.cancel_requested = false;
        st.active_mask = mask;
        st.active_gen = st.next_gen++;
        st.active_user_data = pack_fd(fd, tag_poll, st.active_gen);
        pending_adds_.push_back(pending_add{fd, mask, st.active_user_data});
      }
    }
    wakeup();
  }

  void remove_fd_interest(int fd) noexcept override {
    {
      std::scoped_lock lk{poll_mtx_};
      auto it = polls_.find(fd);
      if (it == polls_.end()) {
        return;
      }

      auto& st = it->second;
      st.desired_mask = 0;
      if (!st.armed) {
        polls_.erase(it);
        return;
      }

      if (!st.cancel_requested) {
        st.cancel_requested = true;
        pending_removes_.push_back(pending_remove{fd, st.active_user_data});
      }
    }
    wakeup();
  }

  auto wait(std::optional<std::chrono::milliseconds> timeout, std::vector<backend_event>& out)
    -> void override {
    out.clear();

    // Drain any interest updates recorded by other threads.
    // These are submitted by the reactor thread to avoid concurrent ring access.
    std::vector<pending_add> pending_adds{};
    std::vector<pending_remove> pending_removes{};
    {
      std::scoped_lock lk{poll_mtx_};
      pending_adds.swap(pending_adds_);
      pending_removes.swap(pending_removes_);
    }
    for (auto const& r : pending_removes) {
      submit_poll_remove(r.fd, r.user_data);
    }
    for (auto const& a : pending_adds) {
      submit_poll_add(a.fd, a.mask, a.user_data);
    }

    io_uring_cqe* first = nullptr;
    int wait_ret = 0;

    if (timeout.has_value()) {
      auto ts = to_timespec(*timeout);
      wait_ret = ::io_uring_wait_cqe_timeout(&ring_, &first, &ts);
    } else {
      wait_ret = ::io_uring_wait_cqe(&ring_, &first);
    }

    if (wait_ret < 0) {
      if (wait_ret == -EINTR || wait_ret == -EAGAIN || wait_ret == -ETIME) {
        return;
      }
      throw std::system_error(-wait_ret, std::generic_category(), "io_uring_wait_cqe failed");
    }

    std::vector<pending_add> local_arms{};
    auto handle_one = [&](io_uring_cqe* cqe) {
      std::uint64_t const data = ::io_uring_cqe_get_data64(cqe);
      std::uint64_t const tag = unpack_tag(data);

      if (tag == tag_wakeup) {
        wakeup_pending_.store(false, std::memory_order_release);
        drain_eventfd(eventfd_);
        arm_wakeup();
        return;
      }
      if (tag == tag_remove) {
        return;
      }

      int const fd = unpack_fd(data);
      std::uint32_t const gen = unpack_gen(data);
      int const res = cqe->res;
      std::uint32_t const ev = (res >= 0) ? static_cast<std::uint32_t>(res) : 0U;
      bool const is_cancelled = (res == -ECANCELED);

      {
        std::scoped_lock lk{poll_mtx_};
        auto it = polls_.find(fd);
        if (it != polls_.end()) {
          auto& st = it->second;
          if (st.armed && st.active_gen == gen) {
            st.armed = false;
            st.cancel_requested = false;
            st.active_user_data = 0;
            st.active_gen = 0;
            st.active_mask = 0;
          }
          if (!st.armed && st.desired_mask != 0) {
            st.armed = true;
            st.cancel_requested = false;
            st.active_mask = st.desired_mask;
            st.active_gen = st.next_gen++;
            st.active_user_data = pack_fd(fd, tag_poll, st.active_gen);
            local_arms.push_back(pending_add{fd, st.active_mask, st.active_user_data});
          }
          if (!st.armed && st.desired_mask == 0) {
            polls_.erase(it);
          }
        }
      }

      if (is_cancelled) {
        return;
      }

      bool const is_error =
        (res < 0) ||
        ((ev & (static_cast<std::uint32_t>(POLLERR) | static_cast<std::uint32_t>(POLLHUP) |
                static_cast<std::uint32_t>(POLLRDHUP))) != 0);

      backend_event e{};
      e.fd = fd;
      e.is_error = is_error;
      e.can_read = is_error || ((ev & static_cast<std::uint32_t>(POLLIN)) != 0);
      e.can_write = is_error || ((ev & static_cast<std::uint32_t>(POLLOUT)) != 0);

      if (is_error) {
        if (res < 0) {
          e.ec = std::error_code{-res, std::generic_category()};
        } else if (ev & static_cast<std::uint32_t>(POLLRDHUP)) {
          e.ec = error::eof;
        } else if (ev & static_cast<std::uint32_t>(POLLHUP)) {
          e.ec = error::eof;
        } else {
          e.ec = error::connection_reset;
        }
      }

      out.push_back(e);
    };

    auto handle_one_and_seen = [&](io_uring_cqe* cqe) {
      struct seen_guard {
        io_uring* ring = nullptr;
        io_uring_cqe* cqe = nullptr;
        ~seen_guard() noexcept {
          if (ring != nullptr && cqe != nullptr) {
            ::io_uring_cqe_seen(ring, cqe);
          }
        }
      };
      seen_guard g{&ring_, cqe};
      handle_one(cqe);
    };

    handle_one_and_seen(first);

    io_uring_cqe* batch[127]{};
    unsigned const n = ::io_uring_peek_batch_cqe(&ring_, batch, 127);
    for (unsigned i = 0; i < n; ++i) {
      if (batch[i] != nullptr) {
        handle_one_and_seen(batch[i]);
      }
    }

    for (auto const& arm : local_arms) {
      submit_poll_add(arm.fd, arm.mask, arm.user_data);
    }

    // Apply any interest updates that arrived while we were handling CQEs.
    pending_adds.clear();
    pending_removes.clear();
    {
      std::scoped_lock lk{poll_mtx_};
      pending_adds.swap(pending_adds_);
      pending_removes.swap(pending_removes_);
    }
    for (auto const& r : pending_removes) {
      submit_poll_remove(r.fd, r.user_data);
    }
    for (auto const& a : pending_adds) {
      submit_poll_add(a.fd, a.mask, a.user_data);
    }

    return;
  }

  void wakeup() noexcept override {
    if (eventfd_ < 0) {
      return;
    }
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
      return;
    }
  }

 private:
  struct pending_add {
    int fd = -1;
    int mask = 0;
    std::uint64_t user_data = 0;
  };
  struct pending_remove {
    int fd = -1;
    std::uint64_t user_data = 0;
  };

  struct uring_poll_state {
    bool armed = false;
    bool cancel_requested = false;
    std::uint32_t active_gen = 0;
    std::uint64_t active_user_data = 0;
    int active_mask = 0;
    int desired_mask = 0;
    std::uint32_t next_gen = 1;
  };

  void arm_wakeup() {
    std::scoped_lock lk{ring_mtx_};
    auto* sqe = ::io_uring_get_sqe(&ring_);
    if (sqe == nullptr) {
      int const submit = ::io_uring_submit(&ring_);
      if (submit < 0) {
        throw std::system_error(-submit, std::generic_category(), "io_uring_submit failed");
      }
      sqe = ::io_uring_get_sqe(&ring_);
      if (sqe == nullptr) {
        throw std::system_error(std::make_error_code(std::errc::no_buffer_space),
                                "io_uring_get_sqe failed");
      }
    }
    ::io_uring_prep_poll_add(sqe, eventfd_, POLLIN | POLLERR | POLLHUP);
    ::io_uring_sqe_set_data64(sqe, pack_fd(0, tag_wakeup));
    int const submit = ::io_uring_submit(&ring_);
    if (submit < 0) {
      throw std::system_error(-submit, std::generic_category(), "io_uring_submit failed");
    }
  }

  void submit_poll_remove(int fd, std::uint64_t user_data) {
    std::scoped_lock lk{ring_mtx_};
    auto* sqe = ::io_uring_get_sqe(&ring_);
    if (sqe == nullptr) {
      (void)::io_uring_submit(&ring_);
      sqe = ::io_uring_get_sqe(&ring_);
      if (sqe == nullptr) {
        return;
      }
    }
    ::io_uring_prep_poll_remove(sqe, user_data);
    ::io_uring_sqe_set_data64(sqe, pack_fd(fd, tag_remove));
    (void)::io_uring_submit(&ring_);
  }

  void submit_poll_add(int fd, int mask, std::uint64_t user_data) {
    std::scoped_lock lk{ring_mtx_};
    auto* sqe = ::io_uring_get_sqe(&ring_);
    if (sqe == nullptr) {
      int const submit = ::io_uring_submit(&ring_);
      if (submit < 0) {
        throw std::system_error(-submit, std::generic_category(), "io_uring_submit failed");
      }
      sqe = ::io_uring_get_sqe(&ring_);
      if (sqe == nullptr) {
        throw std::system_error(std::make_error_code(std::errc::no_buffer_space),
                                "io_uring_get_sqe failed");
      }
    }

    ::io_uring_prep_poll_add(sqe, fd, mask);
    ::io_uring_sqe_set_data64(sqe, user_data);
    int const submit = ::io_uring_submit(&ring_);
    if (submit < 0) {
      throw std::system_error(-submit, std::generic_category(), "io_uring_submit failed");
    }
  }

  io_uring ring_{};
  int eventfd_ = -1;
  std::unordered_map<int, uring_poll_state> polls_{};
  std::mutex poll_mtx_{};
  std::mutex ring_mtx_{};
  std::atomic<bool> wakeup_pending_{false};
  std::vector<pending_add> pending_adds_{};
  std::vector<pending_remove> pending_removes_{};
};

inline auto make_backend() -> std::unique_ptr<backend_interface> {
  return std::make_unique<backend_uring>();
}

}  // namespace iocoro::detail

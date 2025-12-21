#include <xz/io/detail/context/io_context_impl.hpp>
#include <xz/io/detail/executor/executor_guard.hpp>
#include <xz/io/detail/operation/operation_base.hpp>
#include <xz/io/error.hpp>

#include <cerrno>
#include <cstdint>
#include <limits>
#include <system_error>

#include <liburing.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>

namespace xz::io::detail {

struct io_context_impl::backend_impl {
  struct io_uring ring_;
  int eventfd_ = -1;

  struct uring_poll_state {
    bool armed = false;             // a poll_add is currently pending in the kernel
    bool cancel_requested = false;  // a poll_remove has been submitted for the active poll
    std::uint32_t active_gen = 0;   // generation for active_user_data
    std::uint64_t active_user_data = 0;
    int active_mask = 0;         // POLL* mask used for the active poll_add
    int desired_mask = 0;        // latest desired POLL* mask
    std::uint32_t next_gen = 1;  // monotonically increasing generation counter
  };

  std::unordered_map<int, uring_poll_state> uring_polls_;
};

auto io_context_impl::native_handle() const noexcept -> int { return backend_->ring_.ring_fd; }

namespace {

constexpr std::uint64_t tag_poll = 0;    // poll-add completion for an fd
constexpr std::uint64_t tag_wakeup = 1;  // poll-add completion for eventfd wakeup
constexpr std::uint64_t tag_remove = 2;  // poll-remove completion (ignored)

constexpr std::uint64_t fd_shift = 2;
constexpr std::uint64_t gen_shift = 34;  // 2 bits tag + 32 bits fd

auto pack_fd(int fd, std::uint64_t tag, std::uint32_t gen = 0) noexcept -> std::uint64_t {
  return (static_cast<std::uint64_t>(gen) << gen_shift) |
         (static_cast<std::uint64_t>(static_cast<std::uint32_t>(fd)) << fd_shift) | (tag & 0x3ULL);
}

auto unpack_tag(std::uint64_t data) noexcept -> std::uint64_t { return (data & 0x3ULL); }
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
  // Clamp to avoid overflow if an extremely large timeout is requested.
  auto const clamped =
    std::min<long long>(ms.count(), static_cast<long long>(std::numeric_limits<int>::max()));
  __kernel_timespec ts{};
  ts.tv_sec = static_cast<__kernel_time64_t>(clamped / 1000);
  ts.tv_nsec = static_cast<long>(static_cast<long long>(clamped % 1000) * 1000LL * 1000LL);
  return ts;
}

}  // namespace

io_context_impl::io_context_impl() {
  backend_ = std::make_unique<backend_impl>();

  // A small queue depth is sufficient for the current poll-based integration.
  int const ret = ::io_uring_queue_init(256, &backend_->ring_, 0);
  if (ret < 0) {
    throw std::system_error(-ret, std::generic_category(), "io_uring_queue_init failed");
  }

  backend_->eventfd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (backend_->eventfd_ < 0) {
    ::io_uring_queue_exit(&backend_->ring_);
    throw std::system_error(errno, std::generic_category(), "eventfd failed");
  }

  // Arm a poll on the wakeup eventfd so wakeup() can interrupt waits.
  auto* sqe = ::io_uring_get_sqe(&backend_->ring_);
  if (sqe == nullptr) {
    close_if_valid(backend_->eventfd_);
    ::io_uring_queue_exit(&backend_->ring_);
    throw std::system_error(std::make_error_code(std::errc::no_buffer_space),
                            "io_uring_get_sqe failed");
  }
  ::io_uring_prep_poll_add(sqe, backend_->eventfd_, POLLIN | POLLERR | POLLHUP);
  ::io_uring_sqe_set_data64(sqe, pack_fd(0, tag_wakeup));

  int const submit = ::io_uring_submit(&backend_->ring_);
  if (submit < 0) {
    close_if_valid(backend_->eventfd_);
    ::io_uring_queue_exit(&backend_->ring_);
    throw std::system_error(-submit, std::generic_category(), "io_uring_submit failed");
  }
}

io_context_impl::~io_context_impl() {
  stop();
  if (backend_) {
    close_if_valid(backend_->eventfd_);
    ::io_uring_queue_exit(&backend_->ring_);
  }
}

void io_context_impl::backend_update_fd_interest(int fd, bool want_read, bool want_write) {
  int mask = 0;
  if (want_read) {
    mask |= POLLIN;
  }
  if (want_write) {
    mask |= POLLOUT;
  }
  mask |= (POLLERR | POLLHUP | POLLRDHUP);

  // Track per-fd poll state so we never have more than one pending poll_add per fd.
  // If the desired mask changes while a poll is pending, we cancel it via poll_remove
  // and re-arm after we observe the poll completion (often with -ECANCELED).
  std::optional<std::uint64_t> cancel_user_data;
  std::optional<std::uint64_t> arm_user_data;
  int arm_mask = 0;
  {
    std::scoped_lock lk{fd_mutex_};
    auto& st = backend_->uring_polls_[fd];
    st.desired_mask = mask;

    if (st.armed) {
      if (st.active_mask == mask) {
        return;  // already armed with the same mask
      }
      if (!st.cancel_requested) {
        st.cancel_requested = true;
        cancel_user_data = st.active_user_data;
      }
    } else {
      st.armed = true;
      st.cancel_requested = false;
      st.active_mask = mask;
      st.active_gen = st.next_gen++;
      st.active_user_data = pack_fd(fd, tag_poll, st.active_gen);
      arm_user_data = st.active_user_data;
      arm_mask = mask;
    }
  }

  if (cancel_user_data.has_value()) {
    auto* sqe = ::io_uring_get_sqe(&backend_->ring_);
    if (sqe == nullptr) {
      int const submit = ::io_uring_submit(&backend_->ring_);
      if (submit < 0) {
        throw std::system_error(-submit, std::generic_category(), "io_uring_submit failed");
      }
      sqe = ::io_uring_get_sqe(&backend_->ring_);
      if (sqe == nullptr) {
        throw std::system_error(std::make_error_code(std::errc::no_buffer_space),
                                "io_uring_get_sqe failed");
      }
    }

    ::io_uring_prep_poll_remove(sqe, *cancel_user_data);
    ::io_uring_sqe_set_data64(sqe, pack_fd(fd, tag_remove));
    int const submit = ::io_uring_submit(&backend_->ring_);
    if (submit < 0) {
      throw std::system_error(-submit, std::generic_category(), "io_uring_submit failed");
    }
    return;
  }

  if (!arm_user_data.has_value()) {
    return;
  }

  auto* sqe = ::io_uring_get_sqe(&backend_->ring_);
  if (sqe == nullptr) {
    int const submit = ::io_uring_submit(&backend_->ring_);
    if (submit < 0) {
      {
        std::scoped_lock lk{fd_mutex_};
        auto it = backend_->uring_polls_.find(fd);
        if (it != backend_->uring_polls_.end() && it->second.active_user_data == *arm_user_data) {
          it->second.armed = false;
          it->second.cancel_requested = false;
          it->second.active_user_data = 0;
          it->second.active_gen = 0;
          it->second.active_mask = 0;
        }
      }
      throw std::system_error(-submit, std::generic_category(), "io_uring_submit failed");
    }
    sqe = ::io_uring_get_sqe(&backend_->ring_);
    if (sqe == nullptr) {
      {
        std::scoped_lock lk{fd_mutex_};
        auto it = backend_->uring_polls_.find(fd);
        if (it != backend_->uring_polls_.end() && it->second.active_user_data == *arm_user_data) {
          it->second.armed = false;
          it->second.cancel_requested = false;
          it->second.active_user_data = 0;
          it->second.active_gen = 0;
          it->second.active_mask = 0;
        }
      }
      throw std::system_error(std::make_error_code(std::errc::no_buffer_space),
                              "io_uring_get_sqe failed");
    }
  }

  ::io_uring_prep_poll_add(sqe, fd, arm_mask);
  ::io_uring_sqe_set_data64(sqe, *arm_user_data);

  int const submit = ::io_uring_submit(&backend_->ring_);
  if (submit < 0) {
    {
      std::scoped_lock lk{fd_mutex_};
      auto it = backend_->uring_polls_.find(fd);
      if (it != backend_->uring_polls_.end() && it->second.active_user_data == *arm_user_data) {
        it->second.armed = false;
        it->second.cancel_requested = false;
        it->second.active_user_data = 0;
        it->second.active_gen = 0;
        it->second.active_mask = 0;
      }
    }
    throw std::system_error(-submit, std::generic_category(), "io_uring_submit failed");
  }
}

void io_context_impl::backend_remove_fd_interest(int fd) noexcept {
  if (fd < 0) {
    return;
  }

  std::optional<std::uint64_t> cancel_user_data;
  {
    std::scoped_lock lk{fd_mutex_};
    auto it = backend_->uring_polls_.find(fd);
    if (it == backend_->uring_polls_.end()) {
      return;
    }

    auto& st = it->second;
    st.desired_mask = 0;

    if (!st.armed) {
      backend_->uring_polls_.erase(it);
      return;
    }

    if (!st.cancel_requested) {
      st.cancel_requested = true;
      cancel_user_data = st.active_user_data;
    }
  }

  if (!cancel_user_data.has_value()) {
    return;
  }

  auto* sqe = ::io_uring_get_sqe(&backend_->ring_);
  if (sqe == nullptr) {
    (void)::io_uring_submit(&backend_->ring_);
    sqe = ::io_uring_get_sqe(&backend_->ring_);
    if (sqe == nullptr) {
      return;
    }
  }

  ::io_uring_prep_poll_remove(sqe, *cancel_user_data);
  ::io_uring_sqe_set_data64(sqe, pack_fd(fd, tag_remove));
  (void)::io_uring_submit(&backend_->ring_);
}

auto io_context_impl::process_events(std::optional<std::chrono::milliseconds> max_wait)
  -> std::size_t {
  executor_guard g{executor{*this}};

  // Ensure any previously prepared SQEs are submitted before we wait.
  int const submitted = ::io_uring_submit(&backend_->ring_);
  if (submitted < 0) {
    throw std::system_error(-submitted, std::generic_category(), "io_uring_submit failed");
  }

  io_uring_cqe* first = nullptr;
  int wait_ret = 0;

  if (max_wait.has_value()) {
    auto ts = to_timespec(*max_wait);
    wait_ret = ::io_uring_wait_cqe_timeout(&backend_->ring_, &first, &ts);
  } else {
    wait_ret = ::io_uring_wait_cqe(&backend_->ring_, &first);
  }

  if (wait_ret < 0) {
    if (wait_ret == -EINTR || wait_ret == -EAGAIN || wait_ret == -ETIME) {
      return 0;
    }
    throw std::system_error(-wait_ret, std::generic_category(), "io_uring_wait_cqe failed");
  }

  std::size_t count = 0;

  auto handle_one = [&](io_uring_cqe* cqe) {
    std::uint64_t const data = ::io_uring_cqe_get_data64(cqe);
    std::uint64_t const tag = unpack_tag(data);

    if (tag == tag_wakeup) {
      drain_eventfd(backend_->eventfd_);
      // Re-arm wakeup poll.
      auto* sqe = ::io_uring_get_sqe(&backend_->ring_);
      if (sqe != nullptr) {
        ::io_uring_prep_poll_add(sqe, backend_->eventfd_, POLLIN | POLLERR | POLLHUP);
        ::io_uring_sqe_set_data64(sqe, pack_fd(0, tag_wakeup));
        (void)::io_uring_submit(&backend_->ring_);
      }
      return;
    }

    if (tag == tag_remove) {
      return;
    }

    int const fd = unpack_fd(data);
    std::uint32_t const gen = unpack_gen(data);
    int const res = cqe->res;
    std::uint32_t const ev = (res >= 0) ? static_cast<std::uint32_t>(res) : 0U;

    // If we explicitly cancelled a poll (poll_remove) to update the interest mask,
    // the poll_add completion typically arrives with -ECANCELED. This is not an I/O
    // error and must not complete user operations.
    bool const is_cancelled = (res == -ECANCELED);

    bool const is_error =
      (!is_cancelled && (res < 0)) ||
      ((ev & (static_cast<std::uint32_t>(POLLERR) | static_cast<std::uint32_t>(POLLHUP) |
              static_cast<std::uint32_t>(POLLRDHUP))) != 0);

    bool const can_read =
      (!is_cancelled) && (is_error || ((ev & static_cast<std::uint32_t>(POLLIN)) != 0));
    bool const can_write =
      (!is_cancelled) && (is_error || ((ev & static_cast<std::uint32_t>(POLLOUT)) != 0));

    std::unique_ptr<operation_base> read_op;
    std::unique_ptr<operation_base> write_op;
    bool still_want_read = false;
    bool still_want_write = false;

    {
      std::scoped_lock lk{fd_mutex_};
      // Mark the active poll as completed if this CQE corresponds to the currently
      // armed generation for this fd.
      auto pit = backend_->uring_polls_.find(fd);
      if (pit != backend_->uring_polls_.end()) {
        auto& st = pit->second;
        if (st.armed && st.active_gen == gen) {
          st.armed = false;
          st.cancel_requested = false;
          st.active_user_data = 0;
          st.active_gen = 0;
          st.active_mask = 0;
          // Keep st.desired_mask as-is; it may have been updated while pending.
        }
      }

      auto it = fd_operations_.find(fd);
      if (it != fd_operations_.end()) {
        if (!is_cancelled && can_read) {
          read_op = std::move(it->second.read_op);
        }
        if (!is_cancelled && can_write) {
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

    if (is_cancelled) {
      return;
    }

    if (is_error) {
      auto const ec = (res < 0) ? std::error_code{-res, std::generic_category()}
                                : std::make_error_code(std::errc::connection_reset);
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

    seen_guard g{&backend_->ring_, cqe};
    handle_one(cqe);
  };

  // Process the CQE we waited for.
  handle_one_and_seen(first);

  // Then drain any other CQEs already available.
  io_uring_cqe* batch[127]{};
  unsigned const n = ::io_uring_peek_batch_cqe(&backend_->ring_, batch, 127);
  for (unsigned i = 0; i < n; ++i) {
    if (batch[i] != nullptr) {
      handle_one_and_seen(batch[i]);
    }
  }

  return count;
}

void io_context_impl::wakeup() {
  if (!backend_ || backend_->eventfd_ < 0) {
    return;
  }

  std::uint64_t value = 1;
  for (;;) {
    auto const n = ::write(backend_->eventfd_, &value, sizeof(value));
    if (n >= 0) {
      return;
    }
    if (errno == EINTR) {
      continue;
    }
    // Best-effort wakeup: ignore EAGAIN or EBADF races during shutdown.
    return;
  }
}

}  // namespace xz::io::detail

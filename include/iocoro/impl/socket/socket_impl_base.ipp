#include <iocoro/detail/socket/socket_impl_base.hpp>

#include <iocoro/detail/socket_utils.hpp>
#include <iocoro/any_executor.hpp>

#include <unistd.h>

namespace iocoro::detail::socket {

inline auto socket_impl_base::open(int domain, int type, int protocol) noexcept -> std::error_code {
  int fd = -1;
  bool opened = false;
  {
    std::scoped_lock lk{mtx_};
    if (state_ != fd_state::closed || native_handle() >= 0) {
      return error::busy;
    }
    state_ = fd_state::opening;
  }

  fd = ::socket(domain, type, protocol);
  if (fd < 0) {
    std::scoped_lock lk{mtx_};
    if (state_ == fd_state::opening) {
      state_ = fd_state::closed;
    }
    return std::error_code(errno, std::generic_category());
  }

  // Best-effort: set CLOEXEC + non-blocking.
  (void)set_cloexec(fd);
  (void)set_nonblocking(fd);

  {
    std::scoped_lock lk{mtx_};
    if (state_ == fd_state::opening) {
      fd_.store(fd, std::memory_order_release);
      (void)fd_gen_.fetch_add(1, std::memory_order_release);
      state_ = fd_state::open;
      opened = true;
    }
  }

  if (opened) {
    auto const reg_ec = ctx_impl_->arm_fd_interest(fd);
    if (reg_ec) {
      (void)close();
      return reg_ec;
    }
    return {};
  }

  // Aborted by close()/assign() while opening.
  // We intentionally do not adopt the fd.
  (void)::close(fd);
  return error::busy;
}

inline auto socket_impl_base::assign(int fd) noexcept -> std::error_code {
  if (fd < 0) {
    return error::invalid_argument;
  }

  int old_fd = -1;
  bool assigned = false;
  detail::event_handle rh{};
  detail::event_handle wh{};
  {
    std::scoped_lock lk{mtx_};
    // Mark as opening to block concurrent open/assign.
    // Clear current resources (if any); close happens outside the lock.
    if (state_ == fd_state::open) {
      old_fd = fd_.exchange(-1, std::memory_order_acq_rel);
      (void)fd_gen_.fetch_add(1, std::memory_order_release);
      rh = std::exchange(read_handle_, {});
      wh = std::exchange(write_handle_, {});
    }
    state_ = fd_state::opening;
  }

  // Cancel/deregister/close old fd outside lock.
  rh.cancel();
  wh.cancel();
  if (old_fd >= 0) {
    ctx_impl_->disarm_fd_interest(old_fd);
    (void)::close(old_fd);
  }

  // Best-effort flags.
  (void)set_cloexec(fd);
  (void)set_nonblocking(fd);

  {
    std::scoped_lock lk{mtx_};
    if (state_ == fd_state::opening) {
      fd_.store(fd, std::memory_order_release);
      (void)fd_gen_.fetch_add(1, std::memory_order_release);
      state_ = fd_state::open;
      assigned = true;
    }
  }

  if (assigned) {
    auto const reg_ec = ctx_impl_->arm_fd_interest(fd);
    if (reg_ec) {
      (void)close();
      return reg_ec;
    }
    return {};
  }

  // Aborted by close() while assigning.
  (void)::close(fd);
  return error::busy;
}

inline void socket_impl_base::cancel() noexcept {
  detail::event_handle rh{};
  detail::event_handle wh{};
  {
    std::scoped_lock lk{mtx_};
    rh = std::exchange(read_handle_, {});
    wh = std::exchange(write_handle_, {});
  }

  rh.cancel();
  wh.cancel();
}

inline void socket_impl_base::cancel_read() noexcept {
  detail::event_handle rh{};
  {
    std::scoped_lock lk{mtx_};
    rh = std::exchange(read_handle_, {});
  }
  rh.cancel();
}

inline void socket_impl_base::cancel_write() noexcept {
  detail::event_handle wh{};
  {
    std::scoped_lock lk{mtx_};
    wh = std::exchange(write_handle_, {});
  }
  wh.cancel();
}

inline auto socket_impl_base::close() noexcept -> std::error_code {
  int fd = -1;
  detail::event_handle rh{};
  detail::event_handle wh{};
  {
    std::scoped_lock lk{mtx_};
    if (state_ == fd_state::closed) {
      return {};
    }

    // If opening, we only mark closed; the opener will close the fd it created.
    if (state_ == fd_state::opening) {
      state_ = fd_state::closed;
      read_handle_ = {};
      write_handle_ = {};
      fd_.store(-1, std::memory_order_release);
      (void)fd_gen_.fetch_add(1, std::memory_order_release);
      return {};
    }

    // open -> closed
    state_ = fd_state::closed;
    fd = fd_.exchange(-1, std::memory_order_acq_rel);
    (void)fd_gen_.fetch_add(1, std::memory_order_release);
    rh = std::exchange(read_handle_, {});
    wh = std::exchange(write_handle_, {});
  }

  rh.cancel();
  wh.cancel();
  if (fd >= 0) {
    ctx_impl_->disarm_fd_interest(fd);
    for (;;) {
      if (::close(fd) == 0) {
        break;
      }
      if (errno == EINTR) {
        // close() may be interrupted but the fd is no longer usable; treat as success.
        break;
      }
      return std::error_code(errno, std::generic_category());
    }
  }
  return {};
}

inline auto socket_impl_base::release() noexcept -> int {
  int fd = -1;
  detail::event_handle rh{};
  detail::event_handle wh{};
  {
    std::scoped_lock lk{mtx_};
    fd = fd_.exchange(-1, std::memory_order_acq_rel);
    (void)fd_gen_.fetch_add(1, std::memory_order_release);
    state_ = fd_state::closed;
    rh = std::exchange(read_handle_, {});
    wh = std::exchange(write_handle_, {});
  }

  // Cancel any in-flight ops and deregister interest, but do NOT close the fd.
  rh.cancel();
  wh.cancel();
  if (fd >= 0) {
    ctx_impl_->disarm_fd_interest(fd);
  }
  return fd;
}

}  // namespace iocoro::detail::socket

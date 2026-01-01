#include <iocoro/detail/socket/socket_impl_base.hpp>

#include <iocoro/detail/executor_guard.hpp>
#include <iocoro/executor.hpp>

#include <fcntl.h>
#include <unistd.h>

namespace iocoro::detail::socket {

namespace {

inline auto set_nonblocking(int fd) noexcept -> bool {
  int flags = ::fcntl(fd, F_GETFL, 0);
  if (flags < 0) {
    return false;
  }
  if ((flags & O_NONBLOCK) != 0) {
    return true;
  }
  return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

inline auto set_cloexec(int fd) noexcept -> bool {
  int flags = ::fcntl(fd, F_GETFD, 0);
  if (flags < 0) {
    return false;
  }
  if ((flags & FD_CLOEXEC) != 0) {
    return true;
  }
  return ::fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == 0;
}

}  // namespace

inline auto socket_impl_base::open(int domain, int type, int protocol) noexcept -> std::error_code {
  {
    std::scoped_lock lk{mtx_};
    if (state_ != fd_state::closed || native_handle() >= 0) {
      return error::busy;
    }
    state_ = fd_state::opening;
  }

  int fd = ::socket(domain, type, protocol);
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
      state_ = fd_state::open;
      return {};
    }
    // Aborted by close()/assign() while opening.
    // We intentionally do not adopt the fd.
  }

  (void)::close(fd);
  return error::busy;
}

inline auto socket_impl_base::assign(int fd) noexcept -> std::error_code {
  if (fd < 0) {
    return error::invalid_argument;
  }

  int old_fd = -1;
  fd_event_handle rh{};
  fd_event_handle wh{};
  {
    std::scoped_lock lk{mtx_};
    // Mark as opening to block concurrent open/assign.
    // Clear current resources (if any); close happens outside the lock.
    if (state_ == fd_state::open) {
      old_fd = fd_.exchange(-1, std::memory_order_acq_rel);
      rh = std::exchange(read_handle_, {});
      wh = std::exchange(write_handle_, {});
    }
    state_ = fd_state::opening;
  }

  // Cancel/deregister/close old fd outside lock.
  rh.cancel();
  wh.cancel();
  if (old_fd >= 0) {
    (void)::close(old_fd);
  }

  // Best-effort flags.
  (void)set_cloexec(fd);
  (void)set_nonblocking(fd);

  {
    std::scoped_lock lk{mtx_};
    if (state_ == fd_state::opening) {
      fd_.store(fd, std::memory_order_release);
      state_ = fd_state::open;
      return {};
    }
  }

  // Aborted by close() while assigning.
  (void)::close(fd);
  return error::busy;
}

inline void socket_impl_base::cancel() noexcept {
  fd_event_handle rh{};
  fd_event_handle wh{};
  {
    std::scoped_lock lk{mtx_};
    rh = std::exchange(read_handle_, {});
    wh = std::exchange(write_handle_, {});
  }

  rh.cancel();
  wh.cancel();

  /// The fd_event_handle::cancel() method will handle deregistration from the IO loop
  /// if no other operations remain, so explicit deregistration here is unnecessary.
}

inline void socket_impl_base::cancel_read() noexcept {
  fd_event_handle rh{};
  {
    std::scoped_lock lk{mtx_};
    rh = std::exchange(read_handle_, {});
  }
  rh.cancel();
}

inline void socket_impl_base::cancel_write() noexcept {
  fd_event_handle wh{};
  {
    std::scoped_lock lk{mtx_};
    wh = std::exchange(write_handle_, {});
  }
  wh.cancel();
}

inline void socket_impl_base::close() noexcept {
  int fd = -1;
  fd_event_handle rh{};
  fd_event_handle wh{};
  {
    std::scoped_lock lk{mtx_};
    if (state_ == fd_state::closed) {
      return;
    }

    // If opening, we only mark closed; the opener will close the fd it created.
    if (state_ == fd_state::opening) {
      state_ = fd_state::closed;
      read_handle_ = {};
      write_handle_ = {};
      fd_.store(-1, std::memory_order_release);
      return;
    }

    // open -> closed
    state_ = fd_state::closed;
    fd = fd_.exchange(-1, std::memory_order_acq_rel);
    rh = std::exchange(read_handle_, {});
    wh = std::exchange(write_handle_, {});
  }

  rh.cancel();
  wh.cancel();
  if (fd >= 0) {
    (void)::close(fd);
  }
}

inline auto socket_impl_base::release() noexcept -> int {
  int fd = -1;
  fd_event_handle rh{};
  fd_event_handle wh{};
  {
    std::scoped_lock lk{mtx_};
    fd = fd_.exchange(-1, std::memory_order_acq_rel);
    state_ = fd_state::closed;
    rh = std::exchange(read_handle_, {});
    wh = std::exchange(write_handle_, {});
  }

  // Cancel any in-flight ops and deregister interest, but do NOT close the fd.
  rh.cancel();
  wh.cancel();
  return fd;
}

}  // namespace iocoro::detail::socket

#include <iocoro/detail/socket/socket_impl_base.hpp>

#include <iocoro/detail/socket_utils.hpp>

namespace iocoro::detail::socket {

inline auto socket_impl_base::open(int domain, int type, int protocol) noexcept -> result<void> {
  std::scoped_lock lk{lifecycle_mtx_};
  if (res_.load(std::memory_order_acquire)) {
    return fail(error::already_open);
  }

  int fd = ::socket(domain, type, protocol);
  if (fd < 0) {
    return fail(std::error_code(errno, std::generic_category()));
  }

  if (!set_cloexec(fd) || !set_nonblocking(fd)) {
    auto ec = map_socket_errno(errno);
    (void)::close(fd);
    return fail(ec);
  }

  if (!ctx_impl_->add_fd(fd)) {
    (void)::close(fd);
    return fail(error::internal_error);
  }

  auto res = std::make_shared<fd_resource>(ex_, fd);
  res_.store(std::move(res), std::memory_order_release);
  return ok();
}

inline auto socket_impl_base::assign(int fd) noexcept -> result<void> {
  if (fd < 0) {
    return fail(error::invalid_argument);
  }

  std::shared_ptr<fd_resource> old{};
  {
    std::scoped_lock lk{lifecycle_mtx_};
    old = res_.exchange(std::shared_ptr<fd_resource>{}, std::memory_order_acq_rel);
  }

  mark_closing(old);

  if (!set_cloexec(fd) || !set_nonblocking(fd)) {
    auto ec = map_socket_errno(errno);
    (void)::close(fd);
    return fail(ec);
  }

  if (!ctx_impl_->add_fd(fd)) {
    (void)::close(fd);
    return fail(error::internal_error);
  }

  auto res = std::make_shared<fd_resource>(ex_, fd);
  {
    std::scoped_lock lk{lifecycle_mtx_};
    res_.store(std::move(res), std::memory_order_release);
  }
  return ok();
}

inline void socket_impl_base::cancel() noexcept {
  auto res = acquire_resource();
  if (!res) {
    return;
  }
  res->cancel_all_handles();
}

inline void socket_impl_base::cancel_read() noexcept {
  auto res = acquire_resource();
  if (!res) {
    return;
  }
  res->cancel_read_handle();
}

inline void socket_impl_base::cancel_write() noexcept {
  auto res = acquire_resource();
  if (!res) {
    return;
  }
  res->cancel_write_handle();
}

inline auto socket_impl_base::close() noexcept -> result<void> {
  std::shared_ptr<fd_resource> old{};
  {
    std::scoped_lock lk{lifecycle_mtx_};
    old = res_.exchange(std::shared_ptr<fd_resource>{}, std::memory_order_acq_rel);
  }
  mark_closing(old);
  return ok();
}

inline auto socket_impl_base::release() noexcept -> result<int> {
  std::shared_ptr<fd_resource> old{};
  {
    std::scoped_lock lk{lifecycle_mtx_};
    old = res_.load(std::memory_order_acquire);
    if (!old) {
      return unexpected(error::not_open);
    }
    if (old->inflight_count() != 0) {
      return unexpected(error::busy);
    }
    old = res_.exchange(std::shared_ptr<fd_resource>{}, std::memory_order_acq_rel);
  }

  if (!old) {
    return unexpected(error::not_open);
  }

  old->mark_closing();
  old->cancel_all_handles();

  auto fd = old->release_fd();
  if (fd >= 0) {
    ctx_impl_->remove_fd(fd);
  }
  return fd;
}

}  // namespace iocoro::detail::socket

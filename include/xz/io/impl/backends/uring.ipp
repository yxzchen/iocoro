
#include <xz/io/impl/backends/io_context_impl.ipp>

#include <cerrno>
#include <system_error>

#include <liburing.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>

namespace xz::io::detail {

io_context_impl::io_context_impl() {
  int ret = io_uring_queue_init(256, &ring_, 0);
  if (ret < 0) {
    throw std::system_error(-ret, std::generic_category(), "io_uring_queue_init failed");
  }
  owner_thread_.store({}, std::memory_order_release);
}

io_context_impl::~io_context_impl() {
  io_uring_queue_exit(&ring_);
}

void io_context_impl::register_fd_read(int fd, std::unique_ptr<io_context::operation_base> op) {
  std::lock_guard lock(fd_mutex_);
  auto& ops = fd_operations_[fd];
  ops.read_op = std::move(op);

  struct io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
  if (!sqe) {
    fd_operations_.erase(fd);
    throw std::system_error(ENOMEM, std::generic_category(), "io_uring_get_sqe failed");
  }

  io_uring_prep_poll_add(sqe, fd, POLLIN);
  io_uring_sqe_set_data(sqe, reinterpret_cast<void*>(static_cast<uintptr_t>(fd) | (1ULL << 32)));
  io_uring_submit(&ring_);
}

void io_context_impl::register_fd_write(int fd, std::unique_ptr<io_context::operation_base> op) {
  std::lock_guard lock(fd_mutex_);
  auto& ops = fd_operations_[fd];
  ops.write_op = std::move(op);

  struct io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
  if (!sqe) {
    fd_operations_.erase(fd);
    throw std::system_error(ENOMEM, std::generic_category(), "io_uring_get_sqe failed");
  }

  io_uring_prep_poll_add(sqe, fd, POLLOUT);
  io_uring_sqe_set_data(sqe, reinterpret_cast<void*>(static_cast<uintptr_t>(fd) | (2ULL << 32)));
  io_uring_submit(&ring_);
}

void io_context_impl::register_fd_readwrite(int fd, std::unique_ptr<io_context::operation_base> read_op,
                                            std::unique_ptr<io_context::operation_base> write_op) {
  std::lock_guard lock(fd_mutex_);
  auto& ops = fd_operations_[fd];
  ops.read_op = std::move(read_op);
  ops.write_op = std::move(write_op);

  struct io_uring_sqe* sqe_read = io_uring_get_sqe(&ring_);
  struct io_uring_sqe* sqe_write = io_uring_get_sqe(&ring_);
  if (!sqe_read || !sqe_write) {
    fd_operations_.erase(fd);
    throw std::system_error(ENOMEM, std::generic_category(), "io_uring_get_sqe failed");
  }

  io_uring_prep_poll_add(sqe_read, fd, POLLIN);
  io_uring_sqe_set_data(sqe_read, reinterpret_cast<void*>(static_cast<uintptr_t>(fd) | (1ULL << 32)));

  io_uring_prep_poll_add(sqe_write, fd, POLLOUT);
  io_uring_sqe_set_data(sqe_write, reinterpret_cast<void*>(static_cast<uintptr_t>(fd) | (2ULL << 32)));

  io_uring_submit(&ring_);
}

void io_context_impl::deregister_fd(int fd) {
  std::lock_guard lock(fd_mutex_);

  auto it = fd_operations_.find(fd);
  if (it != fd_operations_.end()) {
    if (it->second.read_op) {
      struct io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
      if (sqe) {
        uintptr_t read_user_data = static_cast<uintptr_t>(fd) | (1ULL << 32);
        io_uring_prep_poll_remove(sqe, read_user_data);
        io_uring_sqe_set_data(sqe, nullptr);
      }
    }
    if (it->second.write_op) {
      struct io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
      if (sqe) {
        uintptr_t write_user_data = static_cast<uintptr_t>(fd) | (2ULL << 32);
        io_uring_prep_poll_remove(sqe, write_user_data);
        io_uring_sqe_set_data(sqe, nullptr);
      }
    }
    io_uring_submit(&ring_);
    fd_operations_.erase(it);
  }
}

auto io_context_impl::process_events(std::chrono::milliseconds timeout) -> std::size_t {
  std::size_t count = 0;
  count += process_timers();
  count += process_posted();

  {
    std::lock_guard lock(fd_mutex_);
    if (fd_operations_.empty()) {
      return count;
    }
  }

  struct __kernel_timespec ts;
  ts.tv_sec = timeout.count() / 1000;
  ts.tv_nsec = (timeout.count() % 1000) * 1000000;

  struct io_uring_cqe* cqe;
  int ret = io_uring_wait_cqe_timeout(&ring_, &cqe, timeout.count() < 0 ? nullptr : &ts);

  if (ret == -ETIME || ret == -EINTR) {
    return count;
  }

  if (ret < 0) {
    throw std::system_error(-ret, std::generic_category(), "io_uring_wait_cqe_timeout failed");
  }
  unsigned head;
  unsigned processed = 0;

  io_uring_for_each_cqe(&ring_, head, cqe) {
    ++processed;
    uintptr_t user_data = reinterpret_cast<uintptr_t>(io_uring_cqe_get_data(cqe));

    if (user_data == 0) {
      continue;
    }

    int fd = static_cast<int>(user_data & 0xFFFFFFFF);
    int op_type = static_cast<int>(user_data >> 32);

    std::unique_ptr<io_context::operation_base> op;

    {
      std::lock_guard lock(fd_mutex_);
      auto it = fd_operations_.find(fd);
      if (it != fd_operations_.end()) {
        if (op_type == 1 && it->second.read_op) {
          op = std::move(it->second.read_op);
        } else if (op_type == 2 && it->second.write_op) {
          op = std::move(it->second.write_op);
        }

        if (!it->second.read_op && !it->second.write_op) {
          fd_operations_.erase(it);
        }
      }
    }

    if (op) {
      op->execute();
      ++count;
    }
  }

  io_uring_cq_advance(&ring_, processed);
  return count;
}

void io_context_impl::wakeup() {
  struct io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
  if (sqe) {
    io_uring_prep_nop(sqe);
    io_uring_sqe_set_data(sqe, nullptr);
    io_uring_submit(&ring_);
  }
}

}  // namespace xz::io::detail

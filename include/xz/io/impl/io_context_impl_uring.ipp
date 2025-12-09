#pragma once

#ifdef IOXZ_HAS_URING

#include <xz/io/detail/io_context_impl_uring.hpp>

#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <cerrno>
#include <system_error>

namespace xz::io::detail {

io_context_impl_uring::io_context_impl_uring() {
  // Initialize io_uring with queue depth of 256
  int ret = io_uring_queue_init(256, &ring_, 0);
  if (ret < 0) {
    throw std::system_error(-ret, std::generic_category(), "io_uring_queue_init failed");
  }

  owner_thread_.store({}, std::memory_order_release);
}

io_context_impl_uring::~io_context_impl_uring() {
  io_uring_queue_exit(&ring_);
}

auto io_context_impl_uring::run() -> std::size_t {
  stopped_.store(false, std::memory_order_release);
  owner_thread_.store(std::this_thread::get_id(), std::memory_order_release);

  std::size_t count = 0;
  while (!stopped_.load(std::memory_order_acquire)) {
    count += run_one();
  }

  return count;
}

auto io_context_impl_uring::run_one() -> std::size_t {
  auto timeout = get_timeout();
  return process_events(timeout);
}

auto io_context_impl_uring::run_for(std::chrono::milliseconds timeout) -> std::size_t {
  stopped_.store(false, std::memory_order_release);
  owner_thread_.store(std::this_thread::get_id(), std::memory_order_release);

  auto const deadline = std::chrono::steady_clock::now() + timeout;
  std::size_t count = 0;

  while (!stopped_.load(std::memory_order_acquire)) {
    auto const now = std::chrono::steady_clock::now();
    if (now >= deadline) break;

    auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
    count += process_events(remaining);
  }

  return count;
}

void io_context_impl_uring::stop() {
  stopped_.store(true, std::memory_order_release);
  submit_nop();  // Wake up io_uring_wait_cqe
}

void io_context_impl_uring::restart() {
  stopped_.store(false, std::memory_order_release);
}

void io_context_impl_uring::post(std::function<void()> f) {
  {
    std::lock_guard lock(posted_mutex_);
    posted_operations_.push(std::move(f));
  }
  submit_nop();  // Wake up the ring
}

void io_context_impl_uring::dispatch(std::function<void()> f) {
  if (owner_thread_.load(std::memory_order_acquire) == std::this_thread::get_id()) {
    f();
  } else {
    post(std::move(f));
  }
}

void io_context_impl_uring::register_fd_read(int fd, std::unique_ptr<io_context::operation_base> op) {
  std::lock_guard lock(fd_mutex_);

  auto& ops = fd_operations_[fd];
  ops.read_op = std::move(op);

  // Submit read operation to io_uring
  struct io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
  if (!sqe) {
    fd_operations_.erase(fd);
    throw std::system_error(ENOMEM, std::generic_category(), "io_uring_get_sqe failed");
  }

  // Prep for POLL_ADD to wait for read readiness
  io_uring_prep_poll_add(sqe, fd, POLLIN);
  io_uring_sqe_set_data(sqe, reinterpret_cast<void*>(static_cast<uintptr_t>(fd) | (1ULL << 32)));
  io_uring_submit(&ring_);
}

void io_context_impl_uring::register_fd_write(int fd, std::unique_ptr<io_context::operation_base> op) {
  std::lock_guard lock(fd_mutex_);

  auto& ops = fd_operations_[fd];
  ops.write_op = std::move(op);

  // Submit write poll operation
  struct io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
  if (!sqe) {
    fd_operations_.erase(fd);
    throw std::system_error(ENOMEM, std::generic_category(), "io_uring_get_sqe failed");
  }

  // Prep for POLL_ADD to wait for write readiness
  io_uring_prep_poll_add(sqe, fd, POLLOUT);
  io_uring_sqe_set_data(sqe, reinterpret_cast<void*>(static_cast<uintptr_t>(fd) | (2ULL << 32)));
  io_uring_submit(&ring_);
}

void io_context_impl_uring::register_fd_readwrite(int fd,
                                                   std::unique_ptr<io_context::operation_base> read_op,
                                                   std::unique_ptr<io_context::operation_base> write_op) {
  std::lock_guard lock(fd_mutex_);

  auto& ops = fd_operations_[fd];
  ops.read_op = std::move(read_op);
  ops.write_op = std::move(write_op);

  // Submit both read and write poll operations
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

void io_context_impl_uring::deregister_fd(int fd) {
  std::lock_guard lock(fd_mutex_);

  // Cancel any pending polls for this FD
  struct io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
  if (sqe) {
    io_uring_prep_poll_remove(sqe, static_cast<__u64>(fd));
    io_uring_submit(&ring_);
  }

  fd_operations_.erase(fd);
}

auto io_context_impl_uring::schedule_timer(std::chrono::milliseconds timeout,
                                            std::function<void()> callback) -> timer_handle {
  std::lock_guard lock(timer_mutex_);

  auto const id = next_timer_id_++;
  auto const expiry = std::chrono::steady_clock::now() + timeout;

  auto handle = std::shared_ptr<timer_entry>(new timer_entry{id, expiry, std::move(callback), false});
  timers_.push(handle);

  submit_nop();  // Wake up to recalculate timeout
  return handle;
}

void io_context_impl_uring::cancel_timer(timer_handle handle) {
  if (handle) {
    handle->cancelled.store(true, std::memory_order_release);
  }
}

auto io_context_impl_uring::process_events(std::chrono::milliseconds timeout) -> std::size_t {
  // Process timers first
  process_timers();
  process_posted();

  // Wait for completion events
  struct __kernel_timespec ts;
  ts.tv_sec = timeout.count() / 1000;
  ts.tv_nsec = (timeout.count() % 1000) * 1000000;

  struct io_uring_cqe* cqe;
  int ret = io_uring_wait_cqe_timeout(&ring_, &cqe, timeout.count() < 0 ? nullptr : &ts);

  if (ret == -ETIME || ret == -EINTR) {
    return 0;  // Timeout or interrupted
  }

  if (ret < 0) {
    throw std::system_error(-ret, std::generic_category(), "io_uring_wait_cqe_timeout failed");
  }

  std::size_t count = 0;
  unsigned head;
  unsigned processed = 0;

  io_uring_for_each_cqe(&ring_, head, cqe) {
    ++processed;

    uintptr_t user_data = reinterpret_cast<uintptr_t>(io_uring_cqe_get_data(cqe));

    if (user_data == 0) {
      // NOP operation (used for wakeup)
      continue;
    }

    // Extract FD and operation type from user_data
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

void io_context_impl_uring::process_timers() {
  std::lock_guard lock(timer_mutex_);

  auto const now = std::chrono::steady_clock::now();

  while (!timers_.empty() && timers_.top()->expiry <= now) {
    auto handle = timers_.top();
    timers_.pop();

    if (handle->cancelled.load(std::memory_order_acquire)) {
      continue;
    }

    auto callback = std::move(handle->callback);
    timer_mutex_.unlock();
    callback();
    timer_mutex_.lock();
  }
}

void io_context_impl_uring::process_posted() {
  std::queue<std::function<void()>> ops;

  {
    std::lock_guard lock(posted_mutex_);
    ops.swap(posted_operations_);
  }

  while (!ops.empty()) {
    ops.front()();
    ops.pop();
  }
}

auto io_context_impl_uring::get_timeout() const -> std::chrono::milliseconds {
  std::lock_guard lock(timer_mutex_);

  if (timers_.empty()) {
    return std::chrono::milliseconds(-1);
  }

  auto const now = std::chrono::steady_clock::now();
  auto const& next = timers_.top()->expiry;

  if (next <= now) {
    return std::chrono::milliseconds(0);
  }

  return std::chrono::duration_cast<std::chrono::milliseconds>(next - now);
}

void io_context_impl_uring::submit_nop() {
  struct io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
  if (sqe) {
    io_uring_prep_nop(sqe);
    io_uring_sqe_set_data(sqe, nullptr);  // user_data = 0 for NOP
    io_uring_submit(&ring_);
  }
}

}  // namespace xz::io::detail

#endif  // IOXZ_HAS_URING

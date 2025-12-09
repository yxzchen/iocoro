#pragma once

#include <xz/io/detail/io_context_impl_base.hpp>

#include <sys/epoll.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <unordered_map>
#include <vector>

namespace xz::io::detail {

/// Epoll-based io_context implementation
class io_context_impl_epoll final : public io_context_impl_base {
 public:
  io_context_impl_epoll();
  ~io_context_impl_epoll() override;

  auto run() -> std::size_t override;
  auto run_one() -> std::size_t override;
  auto run_for(std::chrono::milliseconds timeout) -> std::size_t override;

  void stop() override;
  void restart() override;
  auto stopped() const noexcept -> bool override { return stopped_.load(std::memory_order_acquire); }

  void post(std::function<void()> f) override;
  void dispatch(std::function<void()> f) override;

  auto native_handle() const noexcept -> int override { return epoll_fd_; }

  void register_fd_read(int fd, std::unique_ptr<io_context::operation_base> op) override;
  void register_fd_write(int fd, std::unique_ptr<io_context::operation_base> op) override;
  void register_fd_readwrite(int fd, std::unique_ptr<io_context::operation_base> read_op,
                             std::unique_ptr<io_context::operation_base> write_op) override;
  void deregister_fd(int fd) override;

  auto schedule_timer(std::chrono::milliseconds timeout, std::function<void()> callback)
      -> timer_handle override;
  void cancel_timer(timer_handle handle) override;

 private:
  auto process_events(std::chrono::milliseconds timeout) -> std::size_t;
  void process_timers();
  void process_posted();
  auto get_timeout() const -> std::chrono::milliseconds;
  void wakeup();

  int epoll_fd_ = -1;
  int eventfd_ = -1;

  std::atomic<bool> stopped_{false};
  std::atomic<std::thread::id> owner_thread_;

  struct fd_ops {
    std::unique_ptr<io_context::operation_base> read_op;
    std::unique_ptr<io_context::operation_base> write_op;
  };

  std::unordered_map<int, fd_ops> fd_operations_;
  std::mutex fd_mutex_;

  std::priority_queue<timer_handle, std::vector<timer_handle>, std::greater<>> timers_;
  uint64_t next_timer_id_ = 1;
  mutable std::mutex timer_mutex_;

  std::queue<std::function<void()>> posted_operations_;
  std::mutex posted_mutex_;
};

}  // namespace xz::io::detail

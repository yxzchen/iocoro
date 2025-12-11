#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <set>
#include <thread>
#include <unordered_map>
#include <vector>

#ifdef IOXZ_HAS_URING
#include <liburing.h>
#else
#include <sys/epoll.h>
#endif

namespace xz::io::detail {

struct timer_entry {
  uint64_t id;
  std::chrono::steady_clock::time_point expiry;
  std::function<void()> callback;
  std::atomic<bool> cancelled{false};

  auto operator>(timer_entry const& other) const -> bool { return expiry > other.expiry; }
};

using timer_handle = std::shared_ptr<timer_entry>;

/// io_context implementation - uses io_uring if available, otherwise epoll
class io_context_impl {
 public:
  io_context_impl();
  ~io_context_impl();

  auto run() -> std::size_t;
  auto run_one() -> std::size_t;
  auto run_for(std::chrono::milliseconds timeout) -> std::size_t;

  void stop();
  void restart();
  auto stopped() const noexcept -> bool { return stopped_.load(std::memory_order_acquire); }

  void post(std::function<void()> f);
  void dispatch(std::function<void()> f);

#ifdef IOXZ_HAS_URING
  auto native_handle() const noexcept -> int { return ring_.ring_fd; }
#else
  auto native_handle() const noexcept -> int { return epoll_fd_; }
#endif

  void register_fd_read(int fd, std::unique_ptr<io_context::operation_base> op);
  void register_fd_write(int fd, std::unique_ptr<io_context::operation_base> op);
  void register_fd_readwrite(int fd, std::unique_ptr<io_context::operation_base> read_op,
                             std::unique_ptr<io_context::operation_base> write_op);
  void deregister_fd(int fd);

  auto schedule_timer(std::chrono::milliseconds timeout, std::function<void()> callback) -> timer_handle;
  void cancel_timer(timer_handle handle);

 private:
  auto process_events(std::chrono::milliseconds timeout) -> std::size_t;
  void process_timers();
  void process_posted();
  auto get_timeout() -> std::chrono::milliseconds;
  void wakeup();  // Wake up event loop (backend-specific)

#ifdef IOXZ_HAS_URING
  struct io_uring ring_;
#else
  int epoll_fd_ = -1;
  int eventfd_ = -1;
#endif

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

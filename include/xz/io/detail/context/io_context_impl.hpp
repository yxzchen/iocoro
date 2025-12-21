#pragma once

#include <xz/io/detail/timer/timer_entry.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <thread>
#include <unordered_map>
#include <vector>

#ifdef IOCORO_HAS_URING
#include <liburing.h>
#else
#include <sys/epoll.h>
#endif

namespace xz::io::detail {

struct operation_base;

struct timer_entry_compare {
  auto operator()(const std::shared_ptr<timer_entry>& lhs,
                  const std::shared_ptr<timer_entry>& rhs) const noexcept -> bool {
    return lhs->expiry > rhs->expiry;
  }
};

class io_context_impl {
 public:
  io_context_impl();
  ~io_context_impl();

  io_context_impl(io_context_impl const&) = delete;
  auto operator=(io_context_impl const&) -> io_context_impl& = delete;
  io_context_impl(io_context_impl&&) = delete;
  auto operator=(io_context_impl&&) -> io_context_impl& = delete;

  auto run() -> std::size_t;
  auto run_one() -> std::size_t;
  auto run_for(std::chrono::milliseconds timeout) -> std::size_t;

  void stop();
  void restart();
  auto stopped() const noexcept -> bool { return stopped_.load(std::memory_order_acquire); }

  void post(std::function<void()> f);
  void dispatch(std::function<void()> f);

  auto schedule_timer(std::chrono::milliseconds timeout, std::function<void()> callback)
    -> std::shared_ptr<timer_entry>;

  void register_fd_read(int fd, std::unique_ptr<operation_base> op);
  void register_fd_write(int fd, std::unique_ptr<operation_base> op);
  void deregister_fd(int fd);

  void add_work_guard() noexcept;
  void remove_work_guard() noexcept;

  void set_thread_id() noexcept;
  auto running_in_this_thread() const noexcept -> bool;

#ifdef IOCORO_HAS_URING
  auto native_handle() const noexcept -> int { return ring_.ring_fd; }
#else
  auto native_handle() const noexcept -> int { return epoll_fd_; }
#endif

 private:
  auto process_events(std::optional<std::chrono::milliseconds> max_wait = std::nullopt)
    -> std::size_t;
  auto process_timers() -> std::size_t;
  auto process_posted() -> std::size_t;

  auto get_timeout() -> std::chrono::milliseconds;
  void wakeup();

  auto has_work() -> bool;

#ifdef IOCORO_HAS_URING
  struct io_uring ring_;
#else
  int epoll_fd_ = -1;
  int eventfd_ = -1;
#endif

  std::atomic<bool> stopped_{false};

  struct fd_ops {
    std::unique_ptr<operation_base> read_op;
    std::unique_ptr<operation_base> write_op;
  };

  std::unordered_map<int, fd_ops> fd_operations_;
  std::mutex fd_mutex_;

  std::priority_queue<std::shared_ptr<timer_entry>, std::vector<std::shared_ptr<timer_entry>>,
                      timer_entry_compare>
    timers_;
  std::uint64_t next_timer_id_ = 1;
  mutable std::mutex timer_mutex_;

  std::queue<std::function<void()>> posted_operations_;
  std::mutex posted_mutex_;

  std::atomic<std::size_t> work_guard_counter_{0};

  // Thread tracking for executor support
  std::atomic<std::thread::id> thread_id_{std::thread::id{}};
};

}  // namespace xz::io::detail

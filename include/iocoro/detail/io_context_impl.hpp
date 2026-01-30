#pragma once

#include <iocoro/detail/reactor_event.hpp>
#include <iocoro/detail/timer_entry.hpp>
#include <iocoro/detail/unique_function.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <unordered_map>
#include <vector>

namespace iocoro::detail {

struct reactor_op;
using reactor_op_ptr = std::unique_ptr<reactor_op, reactor_op_deleter>;

struct timer_entry_compare {
  auto operator()(const std::shared_ptr<timer_entry>& lhs,
                  const std::shared_ptr<timer_entry>& rhs) const noexcept -> bool {
    return lhs->expiry > rhs->expiry;
  }
};

class io_context_impl {
 public:
  using fd_event_handle = detail::fd_event_handle;
  using timer_event_handle = detail::timer_event_handle;
  using event_desc = detail::event_desc;
  using event_handle = detail::event_handle;

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

  void post(unique_function<void()> f);
  void dispatch(unique_function<void()> f);

  template <class Rep, class Period>
  auto add_timer(std::chrono::duration<Rep, Period> d, reactor_op_ptr op)
    -> timer_event_handle {
    return add_timer(std::chrono::steady_clock::now() + d, std::move(op));
  }
  auto add_timer(std::chrono::steady_clock::time_point expiry,
                 reactor_op_ptr op) -> timer_event_handle;

  /// Cancel a timer registration.
  ///
  /// Thread-safe: can be called from any thread. Completion/abort callbacks
  /// and operation destruction still occur on the reactor thread.
  void cancel_timer(timer_event_handle h) noexcept;

  auto register_event(event_desc desc, reactor_op_ptr op) -> event_handle;

  auto register_fd_read(int fd, reactor_op_ptr op) -> fd_event_handle;
  auto register_fd_write(int fd, reactor_op_ptr op) -> fd_event_handle;
  void deregister_fd(int fd);

  void add_work_guard() noexcept;
  void remove_work_guard() noexcept;

  void set_thread_id() noexcept;
  auto running_in_this_thread() const noexcept -> bool;

 private:
  friend struct detail::fd_event_handle;
  friend struct detail::timer_event_handle;

  struct backend_impl;  // PImpl for the OS backend.

  // Opaque per-thread identity token.
  // Only valid for equality comparison within the process lifetime.
  static auto this_thread_token() noexcept -> std::uintptr_t;

  auto process_events(std::optional<std::chrono::milliseconds> max_wait = std::nullopt)
    -> std::size_t;
  auto process_timers() -> std::size_t;
  auto process_posted() -> std::size_t;

  auto get_timeout() -> std::optional<std::chrono::milliseconds>;
  void wakeup();

  auto has_work() -> bool;

  void reconcile_fd_interest(int fd);

  void backend_update_fd_interest(int fd, bool want_read, bool want_write);
  void backend_remove_fd_interest(int fd) noexcept;

  void cancel_fd_event(int fd, detail::fd_event_kind kind, std::uint64_t token) noexcept;

  std::unique_ptr<backend_impl> backend_;

  std::atomic<bool> stopped_{false};

  struct fd_ops {
    reactor_op_ptr read_op;
    reactor_op_ptr write_op;
    std::uint64_t read_token = detail::fd_event_handle::invalid_token;
    std::uint64_t write_token = detail::fd_event_handle::invalid_token;
  };

  std::mutex fd_mutex_;
  std::unordered_map<int, fd_ops> fd_operations_;
  std::uint64_t next_fd_token_ = 1;
  mutable std::mutex timer_mutex_;
  std::priority_queue<std::shared_ptr<timer_entry>, std::vector<std::shared_ptr<timer_entry>>,
                      timer_entry_compare>
    timers_;
  std::uint64_t next_timer_id_ = 1;

  std::mutex posted_mutex_;
  std::queue<unique_function<void()>> posted_operations_;

  std::atomic<std::size_t> work_guard_counter_{0};

  std::atomic<std::uintptr_t> thread_token_{0};
};

}  // namespace iocoro::detail

#include <iocoro/impl/io_context_impl.ipp>

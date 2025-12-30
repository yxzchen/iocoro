#pragma once

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

struct operation_base;

struct timer_entry_compare {
  auto operator()(const std::shared_ptr<timer_entry>& lhs,
                  const std::shared_ptr<timer_entry>& rhs) const noexcept -> bool {
    return lhs->expiry > rhs->expiry;
  }
};

class io_context_impl {
 public:
  enum class fd_event_kind : std::uint8_t { read, write };

  struct fd_event_handle {
    io_context_impl* impl = nullptr;
    int fd = -1;
    fd_event_kind kind = fd_event_kind::read;
    std::uint64_t token = 0;

    auto valid() const noexcept -> bool {
      return impl != nullptr && fd >= 0 && token != io_context_impl::invalid_fd_token;
    }
    explicit operator bool() const noexcept { return valid(); }

    /// Cancel the registered event iff it is still the same registration.
    /// If the event has been overwritten/replaced, this is a no-op.
    ///
    /// Thread-safe: if called off the context thread, cancellation is posted.
    ///
    /// Lifetime: the referenced io_context_impl must outlive this handle; calling cancel()
    /// after destruction is undefined behavior.
    void cancel() const noexcept;
  };

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

  auto schedule_timer(std::chrono::milliseconds timeout, unique_function<void()> callback)
    -> std::shared_ptr<timer_entry>;

  auto register_fd_read(int fd, std::unique_ptr<operation_base> op) -> fd_event_handle;
  auto register_fd_write(int fd, std::unique_ptr<operation_base> op) -> fd_event_handle;
  void deregister_fd(int fd);

  void add_work_guard() noexcept;
  void remove_work_guard() noexcept;

  void set_thread_id() noexcept;
  auto running_in_this_thread() const noexcept -> bool;

 private:
  struct backend_impl;  // PImpl for the OS backend.

  // Opaque per-thread identity token.
  // Only valid for equality comparison within the process lifetime.
  static auto this_thread_token() noexcept -> std::uintptr_t;

  auto process_events(std::optional<std::chrono::milliseconds> max_wait = std::nullopt)
    -> std::size_t;
  auto process_timers() -> std::size_t;
  auto process_posted() -> std::size_t;

  auto get_timeout() -> std::chrono::milliseconds;
  void wakeup();

  auto has_work() -> bool;

  void reconcile_fd_interest(int fd);
  void reconcile_fd_interest_async(int fd);

  void backend_update_fd_interest(int fd, bool want_read, bool want_write);
  void backend_remove_fd_interest(int fd) noexcept;

  void cancel_fd_event(int fd, fd_event_kind kind, std::uint64_t token) noexcept;

  std::unique_ptr<backend_impl> backend_;

  std::atomic<bool> stopped_{false};

  struct fd_ops {
    std::unique_ptr<operation_base> read_op;
    std::unique_ptr<operation_base> write_op;
    std::uint64_t read_token = 0;
    std::uint64_t write_token = 0;
  };

  std::unordered_map<int, fd_ops> fd_operations_;
  std::mutex fd_mutex_;
  std::uint64_t next_fd_token_ = 1;
  static constexpr std::uint64_t invalid_fd_token = 0;

  std::priority_queue<std::shared_ptr<timer_entry>, std::vector<std::shared_ptr<timer_entry>>,
                      timer_entry_compare>
    timers_;
  std::uint64_t next_timer_id_ = 1;
  mutable std::mutex timer_mutex_;

  std::queue<unique_function<void()>> posted_operations_;
  std::mutex posted_mutex_;

  std::atomic<std::size_t> work_guard_counter_{0};

  std::atomic<std::uintptr_t> thread_token_{0};
};

}  // namespace iocoro::detail

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
#include <variant>
#include <unordered_map>
#include <vector>
#include <type_traits>

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
  enum class fd_event_kind : std::uint8_t { read, write };

  struct fd_event_handle {
    io_context_impl* impl = nullptr;
    int fd = -1;
    fd_event_kind kind = fd_event_kind::read;
    std::uint64_t token = io_context_impl::invalid_fd_token;

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

    static constexpr auto invalid_handle() { return fd_event_handle{}; }
  };

  struct timer_event_handle {
    io_context_impl* impl = nullptr;
    std::shared_ptr<timer_entry> entry;

    auto valid() const noexcept -> bool { return impl != nullptr && entry != nullptr; }
    explicit operator bool() const noexcept { return valid(); }

    /// Cancel the timer (lazy cancellation).
    /// The timer entry is marked as cancelled; actual cleanup happens in process_timers.
    ///
    /// Thread-safe: can be called from any thread.
    void cancel() const noexcept;

    static auto invalid_handle() { return timer_event_handle{}; }
  };

  struct event_desc {
    enum class kind : std::uint8_t { timer, fd_read, fd_write };
    kind type{};
    std::chrono::steady_clock::time_point expiry{};
    int fd{-1};

    static auto timer(std::chrono::steady_clock::time_point tp) noexcept -> event_desc {
      return event_desc{kind::timer, tp, -1};
    }
    static auto fd_read(int handle) noexcept -> event_desc {
      return event_desc{kind::fd_read, {}, handle};
    }
    static auto fd_write(int handle) noexcept -> event_desc {
      return event_desc{kind::fd_write, {}, handle};
    }
  };

  struct event_handle {
    std::variant<std::monostate, timer_event_handle, fd_event_handle> handle{};

    auto valid() const noexcept -> bool {
      return std::visit(
        [](auto const& h) -> bool {
          if constexpr (std::is_same_v<std::decay_t<decltype(h)>, std::monostate>) {
            return false;
          } else {
            return h.valid();
          }
        },
        handle);
    }
    explicit operator bool() const noexcept { return valid(); }

    void cancel() const noexcept {
      std::visit(
        [](auto const& h) noexcept {
          if constexpr (!std::is_same_v<std::decay_t<decltype(h)>, std::monostate>) {
            if (h) {
              h.cancel();
            }
          }
        },
        handle);
    }

    auto as_timer() const noexcept -> timer_event_handle {
      if (auto p = std::get_if<timer_event_handle>(&handle)) {
        return *p;
      }
      return timer_event_handle::invalid_handle();
    }

    auto as_fd() const noexcept -> fd_event_handle {
      if (auto p = std::get_if<fd_event_handle>(&handle)) {
        return *p;
      }
      return fd_event_handle::invalid_handle();
    }
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

  void cancel_fd_event(int fd, fd_event_kind kind, std::uint64_t token) noexcept;

  std::unique_ptr<backend_impl> backend_;

  std::atomic<bool> stopped_{false};

  struct fd_ops {
    reactor_op_ptr read_op;
    reactor_op_ptr write_op;
    std::uint64_t read_token = 0;
    std::uint64_t write_token = 0;
  };

  std::mutex fd_mutex_;
  std::unordered_map<int, fd_ops> fd_operations_;
  std::uint64_t next_fd_token_ = 1;
  static constexpr std::uint64_t invalid_fd_token = 0;

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

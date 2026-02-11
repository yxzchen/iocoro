#pragma once

#include <iocoro/detail/fd_registry.hpp>
#include <iocoro/detail/posted_queue.hpp>
#include <iocoro/detail/reactor_backend.hpp>
#include <iocoro/detail/reactor_types.hpp>
#include <iocoro/detail/timer_registry.hpp>
#include <iocoro/detail/unique_function.hpp>
#include <iocoro/detail/work_guard_counter.hpp>
#include <iocoro/result.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>

namespace iocoro::detail {

class io_context_impl : public std::enable_shared_from_this<io_context_impl> {
 public:
  using event_handle = detail::event_handle;

  // NOTE: io_context_impl must be heap-allocated and shared-owned.
  // Users must construct it with std::make_shared<io_context_impl>().
  // Constructing on stack is not supported.

  io_context_impl();
  explicit io_context_impl(std::unique_ptr<backend_interface> backend);
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
  auto add_timer(std::chrono::duration<Rep, Period> d, reactor_op_ptr op) -> event_handle {
    return add_timer(std::chrono::steady_clock::now() + d, std::move(op));
  }
  auto add_timer(std::chrono::steady_clock::time_point expiry, reactor_op_ptr op) -> event_handle;

  /// Cancel a timer registration.
  ///
  /// Thread-safe: can be called from any thread. Completion/abort callbacks
  /// and operation destruction still occur on the reactor thread.
  void cancel_timer(std::uint32_t index, std::uint64_t generation) noexcept;
  void cancel_event(event_handle h) noexcept;

  auto register_fd_read(int fd, reactor_op_ptr op) -> event_handle;
  auto register_fd_write(int fd, reactor_op_ptr op) -> event_handle;
  void deregister_fd(int fd);

  auto arm_fd_interest(int fd) noexcept -> result<void>;
  void disarm_fd_interest(int fd) noexcept;

  void cancel_fd_event(int fd, detail::fd_event_kind kind, std::uint64_t token) noexcept;

  void add_work_guard() noexcept;
  void remove_work_guard() noexcept;

  void set_thread_id() noexcept;
  auto running_in_this_thread() const noexcept -> bool;

 private:
  // Opaque per-thread identity token.
  // Only valid for equality comparison within the process lifetime.
  static auto this_thread_token() noexcept -> std::uintptr_t;

  auto process_events(std::optional<std::chrono::milliseconds> max_wait = std::nullopt)
    -> std::size_t;
  auto process_timers() -> std::size_t;
  auto process_posted() -> std::size_t;

  auto next_wait(std::optional<std::chrono::steady_clock::time_point> deadline)
    -> std::optional<std::chrono::milliseconds>;
  void wakeup();

  auto is_stopped() const noexcept -> bool;
  auto has_work() -> bool;

  // Execute `f` on the reactor thread (registry/backend ownership thread).
  //
  // INVARIANT: registry/backend mutations and reactor op callbacks happen only on the reactor
  // thread; this is the sole serialization mechanism for reactor state.
  //
  // Semantics:
  // - If already on the reactor thread, executes inline (even if stopped).
  // - Otherwise, enqueues via `post()` and executes on the next event-loop iteration.
  // SAFETY: uses `weak_from_this()` to avoid self-owning cycles while still pinning lifetime
  // during callback execution.
  void dispatch_reactor(unique_function<void(io_context_impl&)> f) noexcept;

  // Abort a reactor op. Must only be called on the reactor thread.
  static void abort_op(reactor_op_ptr op, std::error_code ec) noexcept;

  void apply_fd_interest(int fd, fd_interest interest);

  std::unique_ptr<backend_interface> backend_;

  std::atomic<bool> stopped_{false};
  // True while a thread is inside run/run_one/run_for. This is used to reject
  // concurrent event-loop execution for the same io_context_impl instance.
  std::atomic<bool> running_{false};

  fd_registry fd_registry_{};
  timer_registry timers_{};
  posted_queue posted_{};
  work_guard_counter work_guard_{};
  std::vector<backend_event> backend_events_{};
  std::atomic<std::uintptr_t> thread_token_{0};
};

}  // namespace iocoro::detail

#include <iocoro/impl/io_context_impl.ipp>

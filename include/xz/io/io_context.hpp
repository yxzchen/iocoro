#pragma once

#include <xz/io/detail/operation/operation_base.hpp>
#include <xz/io/timer_handle.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>

namespace xz::io {

class executor;

namespace detail {
class io_context_impl;
}  // namespace detail

/// The execution context for asynchronous I/O operations
class io_context {
 public:
  io_context();
  ~io_context();

  io_context(io_context const&) = delete;
  auto operator=(io_context const&) -> io_context& = delete;
  io_context(io_context&&) = delete;
  auto operator=(io_context&&) -> io_context& = delete;

  auto run() -> std::size_t;
  auto run_one() -> std::size_t;
  auto run_for(std::chrono::milliseconds timeout) -> std::size_t;

  void stop();
  void restart();
  auto stopped() const noexcept -> bool;

  void post(std::function<void()> f);
  void dispatch(std::function<void()> f);

  auto native_handle() const noexcept -> int;

  auto schedule_timer(std::chrono::milliseconds timeout, std::function<void()> callback)
    -> timer_handle;

  /// Get an executor associated with this io_context
  auto get_executor() noexcept -> executor;

  /// Check if the calling thread is running in this io_context
  auto running_in_this_thread() const noexcept -> bool;

  void add_work_guard() noexcept;
  void remove_work_guard() noexcept;

 private:
  void register_fd_read(int fd, std::unique_ptr<detail::operation_base> op);
  void register_fd_write(int fd, std::unique_ptr<detail::operation_base> op);
  void deregister_fd(int fd);

  std::unique_ptr<detail::io_context_impl> impl_;
};

}  // namespace xz::io

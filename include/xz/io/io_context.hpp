#pragma once

#include <atomic>
#include <chrono>
#include <coroutine>
#include <cstdint>
#include <functional>
#include <memory>
#include <queue>
#include <system_error>

namespace xz::io {

namespace detail {
class io_context_impl;
struct timer_entry;
using timer_handle = std::shared_ptr<timer_entry>;
}  // namespace detail

/// The execution context for asynchronous I/O operations
class io_context {
 public:
  io_context();
  ~io_context();

  io_context(io_context const&) = delete;
  auto operator=(io_context const&) -> io_context& = delete;
  io_context(io_context&&) noexcept;
  auto operator=(io_context&&) noexcept -> io_context&;

  auto run() -> std::size_t;

  auto run_one() -> std::size_t;

  auto run_for(std::chrono::milliseconds timeout) -> std::size_t;

  void stop();

  void restart();

  auto stopped() const noexcept -> bool;

  void post(std::function<void()> f);

  void dispatch(std::function<void()> f);

  auto native_handle() const noexcept -> int;

 public:
  struct operation_base {
    virtual ~operation_base() = default;
    virtual void execute() = 0;
  };

  void register_fd_read(int fd, std::unique_ptr<operation_base> op);
  void register_fd_write(int fd, std::unique_ptr<operation_base> op);
  void register_fd_readwrite(int fd, std::unique_ptr<operation_base> read_op, std::unique_ptr<operation_base> write_op);
  void deregister_fd(int fd);

  auto schedule_timer(std::chrono::milliseconds timeout, std::function<void()> callback) -> detail::timer_handle;
  void cancel_timer(detail::timer_handle handle);

 private:
  std::unique_ptr<detail::io_context_impl> impl_;
};

}  // namespace xz::io

#pragma once

#include <xz/io/io_context.hpp>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>

namespace xz::io::detail {

struct timer_entry {
  uint64_t id;
  std::chrono::steady_clock::time_point expiry;
  std::function<void()> callback;
  std::atomic<bool> cancelled{false};

  auto operator>(timer_entry const& other) const -> bool {
    return expiry > other.expiry;
  }
};

using timer_handle = std::shared_ptr<timer_entry>;

/// Base interface for io_context implementations
class io_context_impl_base {
 public:
  virtual ~io_context_impl_base() = default;

  virtual auto run() -> std::size_t = 0;
  virtual auto run_one() -> std::size_t = 0;
  virtual auto run_for(std::chrono::milliseconds timeout) -> std::size_t = 0;

  virtual void stop() = 0;
  virtual void restart() = 0;
  virtual auto stopped() const noexcept -> bool = 0;

  virtual void post(std::function<void()> f) = 0;
  virtual void dispatch(std::function<void()> f) = 0;

  virtual auto native_handle() const noexcept -> int = 0;

  virtual void register_fd_read(int fd, std::unique_ptr<io_context::operation_base> op) = 0;
  virtual void register_fd_write(int fd, std::unique_ptr<io_context::operation_base> op) = 0;
  virtual void register_fd_readwrite(int fd, std::unique_ptr<io_context::operation_base> read_op,
                                     std::unique_ptr<io_context::operation_base> write_op) = 0;
  virtual void deregister_fd(int fd) = 0;

  virtual auto schedule_timer(std::chrono::milliseconds timeout, std::function<void()> callback)
      -> timer_handle = 0;
  virtual void cancel_timer(timer_handle handle) = 0;
};

/// Factory function to create the best available implementation
auto make_io_context_impl() -> std::unique_ptr<io_context_impl_base>;

}  // namespace xz::io::detail

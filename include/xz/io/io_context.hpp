#pragma once

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

  /// Get an executor associated with this io_context
  auto get_executor() noexcept -> executor;

 private:
  std::unique_ptr<detail::io_context_impl> impl_;
};

}  // namespace xz::io

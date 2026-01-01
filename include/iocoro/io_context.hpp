#pragma once

#include <iocoro/detail/io_context_impl.hpp>
#include <iocoro/io_executor.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>

namespace iocoro {

/// The execution context for asynchronous I/O operations
class io_context {
 public:
  io_context() : impl_(std::make_unique<detail::io_context_impl>()) {}
  ~io_context() = default;

  io_context(io_context const&) = delete;
  auto operator=(io_context const&) -> io_context& = delete;
  io_context(io_context&&) = delete;
  auto operator=(io_context&&) -> io_context& = delete;

  auto run() -> std::size_t { return impl_->run(); }
  auto run_one() -> std::size_t { return impl_->run_one(); }
  auto run_for(std::chrono::milliseconds timeout) -> std::size_t { return impl_->run_for(timeout); }

  void stop() { impl_->stop(); }
  void restart() { impl_->restart(); }
  auto stopped() const noexcept -> bool { return impl_->stopped(); }

  /// Get an io_executor associated with this io_context
  auto get_executor() noexcept -> io_executor { return io_executor{*impl_}; }

 private:
  std::unique_ptr<detail::io_context_impl> impl_;
};

}  // namespace iocoro

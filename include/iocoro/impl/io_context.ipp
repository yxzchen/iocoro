#include <iocoro/detail/io_context_impl.hpp>
#include <iocoro/executor.hpp>
#include <iocoro/io_context.hpp>
#include <iocoro/timer_handle.hpp>

#include <utility>

namespace iocoro {

io_context::io_context() : impl_(std::make_unique<detail::io_context_impl>()) {}

io_context::~io_context() = default;

inline auto io_context::run() -> std::size_t { return impl_->run(); }

inline auto io_context::run_one() -> std::size_t { return impl_->run_one(); }

inline auto io_context::run_for(std::chrono::milliseconds timeout) -> std::size_t {
  return impl_->run_for(timeout);
}

inline void io_context::stop() { impl_->stop(); }

inline void io_context::restart() { impl_->restart(); }

inline auto io_context::stopped() const noexcept -> bool { return impl_->stopped(); }

inline auto io_context::get_executor() noexcept -> executor { return executor{*impl_}; }

}  // namespace iocoro

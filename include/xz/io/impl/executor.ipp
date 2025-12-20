#pragma once

#include <xz/io/detail/context/io_context_impl.hpp>
#include <xz/io/executor.hpp>
#include <xz/io/io_context.hpp>

#include <stdexcept>

namespace xz::io {

inline executor::executor(detail::io_context_impl& impl) noexcept : impl_{&impl} {}

inline void executor::execute(std::function<void()> f) const {
  if (!impl_) {
    throw std::runtime_error("Invalid executor: no associated context");
  }
  // execute is equivalent to post - queued for later execution, never inline
  impl_->post(std::move(f));
}

inline void executor::post(std::function<void()> f) const {
  if (!impl_) {
    throw std::runtime_error("Invalid executor: no associated context");
  }
  impl_->post(std::move(f));
}

inline void executor::dispatch(std::function<void()> f) const {
  if (!impl_) {
    throw std::runtime_error("Invalid executor: no associated context");
  }
  impl_->dispatch(std::move(f));
}

inline auto executor::running_in_this_thread() const noexcept -> bool {
  if (!impl_) {
    return false;
  }
  return impl_->running_in_this_thread();
}

}  // namespace xz::io


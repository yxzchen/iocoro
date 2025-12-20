#pragma once

#include <xz/io/detail/context/io_context_impl.hpp>
#include <xz/io/executor.hpp>
#include <xz/io/io_context.hpp>

#include <stdexcept>

namespace xz::io {

inline executor::executor(detail::io_context_impl& impl) noexcept : impl_{&impl} {}

inline void executor::execute(std::function<void()> f) const {
  // execute is equivalent to post - queued for later execution, never inline
  impl_->post(std::move(f));
}

inline void executor::post(std::function<void()> f) const { impl_->post(std::move(f)); }

inline void executor::dispatch(std::function<void()> f) const { impl_->dispatch(std::move(f)); }

}  // namespace xz::io

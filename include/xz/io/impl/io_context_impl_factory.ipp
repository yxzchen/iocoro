#pragma once

#include <xz/io/detail/io_context_impl_base.hpp>
#include <xz/io/detail/io_context_impl_epoll.hpp>

#ifdef IOXZ_HAS_URING
#include <xz/io/detail/io_context_impl_uring.hpp>
#endif

namespace xz::io::detail {

auto make_io_context_impl() -> std::unique_ptr<io_context_impl_base> {
#ifdef IOXZ_HAS_URING
  // Try io_uring first (available and kernel supports it)
  try {
    return std::make_unique<io_context_impl_uring>();
  } catch (...) {
    // Fall back to epoll if io_uring fails (old kernel, no permission, etc.)
  }
#endif

  // Use epoll as fallback or default
  return std::make_unique<io_context_impl_epoll>();
}

}  // namespace xz::io::detail

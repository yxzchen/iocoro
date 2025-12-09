#pragma once

#include <xz/io/detail/io_context_impl.hpp>

#ifdef IOXZ_HAS_URING
#include <xz/io/impl/io_context_impl_uring.ipp>
#else
#include <xz/io/impl/io_context_impl_epoll.ipp>
#endif

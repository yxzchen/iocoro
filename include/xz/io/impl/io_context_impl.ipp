
#include <xz/io/detail/io_context_impl.hpp>

#ifdef IOXZ_HAS_URING
#include <xz/io/impl/backends/io_context_uring.ipp>
#else
#include <xz/io/impl/backends/io_context_epoll.ipp>
#endif

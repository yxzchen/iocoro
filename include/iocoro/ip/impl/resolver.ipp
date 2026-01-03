#pragma once

#include <mutex>

namespace iocoro::ip {

namespace detail {

/// Static thread_pool for DNS resolution.
/// Created lazily and never destroyed (lives for the process lifetime).
inline auto get_default_resolver_pool() -> thread_pool& {
  static thread_pool pool{1};
  return pool;
}

}  // namespace detail

template <class Protocol>
auto resolver<Protocol>::get_pool_executor() const -> any_executor {
  if (pool_ex_) {
    return *pool_ex_;
  }
  return any_executor{detail::get_default_resolver_pool().get_executor()};
}

template <class Protocol>
auto resolver<Protocol>::async_resolve(std::string host, std::string service)
  -> awaitable<expected<results_type, std::error_code>> {
  // Reset cancellation flag for this new operation.
  cancelled_->store(false, std::memory_order_release);

  // Capture the calling coroutine's executor for resumption.
  auto io_ex = co_await this_coro::executor;

  // Get the pool executor (custom or default static pool).
  auto pool_ex = get_pool_executor();

  // Create and await the resolve_awaiter with explicit constructor.
  co_return co_await resolve_awaiter{std::move(io_ex), std::move(pool_ex), std::move(host),
                                      std::move(service), cancelled_};
}

}  // namespace iocoro::ip

#pragma once

namespace iocoro::ip {

template <class Protocol>
auto resolver<Protocol>::async_resolve(std::string host, std::string service)
  -> awaitable<expected<results_type, std::error_code>> {
  // Reset cancellation flag for this new operation.
  cancelled_->store(false, std::memory_order_release);

  // Create and await the resolve_awaiter with explicit constructor.
  co_return co_await resolve_awaiter{io_ex_, pool_ex_, std::move(host), std::move(service),
                                      cancelled_};
}

}  // namespace iocoro::ip

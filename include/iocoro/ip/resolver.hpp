#pragma once

// IP-domain resolver.
//
// Resolver is inherently IP-specific (host/service resolution), so it lives under `iocoro::ip`.

#include <iocoro/any_executor.hpp>
#include <iocoro/awaitable.hpp>
#include <iocoro/error.hpp>
#include <iocoro/expected.hpp>
#include <iocoro/io_executor.hpp>
#include <iocoro/thread_pool.hpp>

#include <atomic>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#include <netdb.h>
#include <sys/socket.h>

namespace iocoro::ip {

/// Protocol-typed resolver facade.
///
/// Responsibility boundary (locked-in):
/// - Accept host/service strings.
/// - Produce a list of `Protocol::endpoint` results.
/// - All protocol typing is via the `Protocol` template parameter.
///
/// Threading model:
/// - DNS resolution (getaddrinfo) is executed on a thread_pool executor (blocking call).
/// - Coroutine resumption happens on the calling coroutine's executor.
///
/// Lifecycle:
/// - Optionally accepts a custom thread_pool executor for DNS operations.
/// - If not provided, uses an internal static thread_pool with 1 worker thread.
///
/// Usage:
///   // Use default internal thread_pool:
///   ip::tcp::resolver resolver;
///   auto result = co_await resolver.async_resolve("www.example.com", "80");
///
///   // Use custom thread_pool:
///   thread_pool pool{4};
///   ip::tcp::resolver resolver{pool.get_executor()};
///   auto result = co_await resolver.async_resolve("www.example.com", "80");
template <class Protocol>
class resolver {
 public:
  using protocol_type = Protocol;
  using endpoint = typename Protocol::endpoint;
  using results_type = std::vector<endpoint>;

  /// Construct a resolver with optional custom executor for DNS resolution.
  ///
  /// Parameters:
  /// - pool_ex: Optional executor for running blocking DNS operations (getaddrinfo).
  ///            If not provided, uses an internal static thread_pool with 1 worker thread.
  ///            DNS resolution is a blocking system call that should NOT run on the io_context
  ///            thread. The resolver will post getaddrinfo work to this executor.
  resolver() noexcept = default;
  explicit resolver(any_executor pool_ex) noexcept : pool_ex_(std::move(pool_ex)) {}

  resolver(resolver const&) = delete;
  auto operator=(resolver const&) -> resolver& = delete;
  resolver(resolver&&) noexcept = default;
  auto operator=(resolver&&) noexcept -> resolver& = default;

  /// Resolve a host and service to a list of endpoints.
  ///
  /// Parameters:
  /// - host: Hostname or IP address (e.g., "www.example.com", "192.0.2.1", or empty for passive).
  /// - service: Service name or port number (e.g., "http", "80").
  ///
  /// Returns:
  /// - Success: results_type (may be empty if no addresses were resolved).
  /// - Failure: std::error_code (from getaddrinfo or operation_aborted on cancel).
  ///
  /// Note: This function uses getaddrinfo, which is a blocking system call. To avoid blocking
  /// the io_context thread, the call is executed on the thread_pool provided at construction
  /// (or the internal default thread_pool if none was provided).
  auto async_resolve(std::string host, std::string service)
    -> awaitable<expected<results_type, std::error_code>> {
    // Reset cancellation flag for this new operation.
    cancelled_->store(false, std::memory_order_release);

    // Get the pool executor (custom or default static pool).
    auto pool_ex = pool_ex_ ? *pool_ex_ : get_default_executor();

    // Create and await the resolve_awaiter with explicit constructor.
    co_return co_await resolve_awaiter{std::move(pool_ex), std::move(host), std::move(service),
                                       cancelled_};
  }

  /// Cancel the pending resolve operation (best-effort).
  ///
  /// Note: getaddrinfo itself is not cancellable. This sets a flag that causes the
  /// awaiter to return operation_aborted if checked before the result is processed.
  void cancel() noexcept { cancelled_->store(true, std::memory_order_release); }

 private:
  struct resolve_awaiter;

  /// Get the executor for running blocking DNS operations.
  /// Returns custom executor if provided, otherwise returns the static default thread_pool
  /// executor.
  auto get_default_executor() const -> any_executor {
    static thread_pool pool{1};
    return pool.get_executor();
  }

  std::optional<any_executor> pool_ex_;  // Optional custom executor for blocking DNS calls
  std::shared_ptr<std::atomic<bool>> cancelled_{std::make_shared<std::atomic<bool>>(false)};
};

template <class Protocol>
struct resolver<Protocol>::resolve_awaiter {
  any_executor pool_ex;
  std::string host;
  std::string service;
  std::shared_ptr<std::atomic<bool>> cancelled;

  // Shared state between thread_pool worker and awaiting coroutine.
  struct result_state {
    std::coroutine_handle<> continuation;
    expected<results_type, std::error_code> result{unexpected(error::not_implemented)};
  };
  std::shared_ptr<result_state> state;

  // Explicit constructor to ensure proper initialization.
  explicit resolve_awaiter(any_executor pool_ex_, std::string host_, std::string service_,
                           std::shared_ptr<std::atomic<bool>> cancelled_)
      : pool_ex(std::move(pool_ex_)),
        host(std::move(host_)),
        service(std::move(service_)),
        cancelled(std::move(cancelled_)),
        state(std::make_shared<result_state>()) {}

  bool await_ready() const noexcept { return false; }

  void await_suspend(std::coroutine_handle<> h) {
    state->continuation = h;

    // Build hints for getaddrinfo based on Protocol.
    addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;  // Accept both IPv4 and IPv6.
    hints.ai_socktype = Protocol::type();
    hints.ai_protocol = Protocol::protocol();

    auto host_copy = host;
    auto service_copy = service;

    auto io_ex = ::iocoro::detail::get_current_executor();

    pool_ex.post([state = state, io_ex = io_ex, host_copy = std::move(host_copy),
                  service_copy = std::move(service_copy), hints, cancelled = cancelled]() {
      // Execute getaddrinfo (blocking system call).
      addrinfo* result_list = nullptr;
      int const ret =
        ::getaddrinfo(host_copy.empty() ? nullptr : host_copy.c_str(),
                      service_copy.empty() ? nullptr : service_copy.c_str(), &hints, &result_list);

      // Check cancellation flag before processing results.
      if (cancelled->load(std::memory_order_acquire)) {
        if (result_list) {
          ::freeaddrinfo(result_list);
        }
        state->result = unexpected(error::operation_aborted);
      } else if (ret != 0) {
        // getaddrinfo error.
        // Map EAI_* error codes to std::error_code.
        // For now, use generic_category. A proper implementation could define
        // a custom error category for getaddrinfo errors.
        state->result = unexpected(std::error_code(ret, std::generic_category()));
      } else {
        // Success: convert addrinfo list to Protocol::endpoint list.
        results_type endpoints;
        for (auto* ai = result_list; ai != nullptr; ai = ai->ai_next) {
          auto ep_result = endpoint::from_native(ai->ai_addr, ai->ai_addrlen);
          if (ep_result) {
            endpoints.push_back(std::move(*ep_result));
          }
          // Silently skip addresses that cannot be converted (e.g., unsupported family).
        }
        ::freeaddrinfo(result_list);
        state->result = std::move(endpoints);
      }

      // Post coroutine resumption back to the captured io_executor.
      io_ex.post([state]() { state->continuation.resume(); });
    });
  }

  auto await_resume() -> expected<results_type, std::error_code> {
    return std::move(state->result);
  }
};

}  // namespace iocoro::ip

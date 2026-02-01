#pragma once

// IP-domain resolver.
//
// Resolver is inherently IP-specific (host/service resolution), so it lives under `iocoro::ip`.

#include <iocoro/any_executor.hpp>
#include <iocoro/awaitable.hpp>
#include <iocoro/assert.hpp>
#include <iocoro/error.hpp>
#include <iocoro/expected.hpp>
#include <iocoro/detail/unique_function.hpp>
#include <iocoro/thread_pool.hpp>

#include <atomic>
#include <cstring>
#include <memory>
#include <stop_token>
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
  /// - Failure: std::error_code (from getaddrinfo).
  ///
  /// Cancellation:
  /// - Cancellation is best-effort: the coroutine can be canceled, but the underlying
  ///   getaddrinfo() call will still run to completion on the thread_pool executor.
  ///
  /// Note: This function uses getaddrinfo, which is a blocking system call. To avoid blocking
  /// the io_context thread, the call is executed on the thread_pool provided at construction
  /// (or the internal default thread_pool if none was provided).
  auto async_resolve(std::string host, std::string service)
    -> awaitable<expected<results_type, std::error_code>> {
    // Get the pool executor (custom or default static pool).
    auto pool_ex = pool_ex_ ? *pool_ex_ : get_default_executor();

    // Create and await the resolve_awaiter with explicit constructor.
    co_return co_await resolve_awaiter{std::move(pool_ex), std::move(host), std::move(service)};
  }

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
};

class addrinfo_error_category_impl : public std::error_category {
 public:
  auto name() const noexcept -> char const* override { return "addrinfo"; }

  auto message(int ev) const -> std::string override {
    // Use gai_strerror to get the error message for getaddrinfo errors.
    char const* msg = ::gai_strerror(ev);
    return msg ? msg : "unknown addrinfo error";
  }
};

inline auto addrinfo_error_category() -> std::error_category const& {
  static addrinfo_error_category_impl instance;
  return instance;
}

template <class Protocol>
struct resolver<Protocol>::resolve_awaiter {
  any_executor pool_ex;
  std::string host;
  std::string service;

  // Shared state between thread_pool worker and awaiting coroutine.
  struct result_state {
    std::coroutine_handle<> continuation;
    any_executor ex;
    expected<results_type, std::error_code> result{};
    std::atomic<bool> done{false};
    std::unique_ptr<std::stop_callback<::iocoro::detail::unique_function<void()>>> stop_cb{};
  };
  std::shared_ptr<result_state> state;

  // Explicit constructor to ensure proper initialization.
  explicit resolve_awaiter(any_executor pool_ex_, std::string host_, std::string service_)
      : pool_ex(std::move(pool_ex_)),
        host(std::move(host_)),
        service(std::move(service_)),
        state(std::make_shared<result_state>()) {}

  bool await_ready() const noexcept { return false; }

  template <class Promise>
    requires requires(Promise& p) { p.get_executor(); }
  void await_suspend(std::coroutine_handle<Promise> h) {
    state->continuation = h;
    state->ex = h.promise().get_executor();
    IOCORO_ENSURE(state->ex, "resolver: empty continuation executor");

    auto host_copy = host;
    auto service_copy = service;
    auto st = state;
    auto weak_state = std::weak_ptr<result_state>{st};

    if constexpr (requires {
                    h.promise().get_stop_token();
                  }) {
      auto token = h.promise().get_stop_token();
      if (token.stop_requested()) {
        if (!st->done.exchange(true)) {
          st->result = unexpected(error::operation_aborted);
          st->ex.post([st]() { st->continuation.resume(); });
        }
      } else {
        st->stop_cb =
          std::make_unique<std::stop_callback<::iocoro::detail::unique_function<void()>>>(
            token, ::iocoro::detail::unique_function<void()>{[weak_state]() mutable {
              auto st = weak_state.lock();
              if (!st) {
                return;
              }
              if (st->done.exchange(true)) {
                return;
              }
              st->result = unexpected(error::operation_aborted);
              st->ex.post([st]() { st->continuation.resume(); });
            }});
      }
    }

    pool_ex.post(
      [st, host_copy = std::move(host_copy), service_copy = std::move(service_copy)]() {
        expected<results_type, std::error_code> result{};
        try {
          // Build hints for getaddrinfo based on Protocol.
          addrinfo hints;
          std::memset(&hints, 0, sizeof(hints));
          hints.ai_family = AF_UNSPEC;  // Accept both IPv4 and IPv6.
          hints.ai_socktype = Protocol::type();
          hints.ai_protocol = Protocol::protocol();

          // Execute getaddrinfo (blocking system call).
          addrinfo* result_list = nullptr;
          int const ret = ::getaddrinfo(host_copy.empty() ? nullptr : host_copy.c_str(),
                                        service_copy.empty() ? nullptr : service_copy.c_str(),
                                        &hints, &result_list);

          if (ret != 0) {
            // getaddrinfo error.
            // Map EAI_* error codes to addrinfo_error and create a proper error_code.
            result = unexpected(std::error_code(ret, addrinfo_error_category()));
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
            result = std::move(endpoints);
          }
        } catch (...) {
          result = unexpected(error::internal_error);
        }

        if (st->done.exchange(true)) {
          return;
        }

        st->result = std::move(result);
        st->stop_cb.reset();
        // Post coroutine resumption back to the captured IO executor.
        st->ex.post([st]() { st->continuation.resume(); });
      });
  }

  auto await_resume() -> expected<results_type, std::error_code> {
    return std::move(state->result);
  }
};

}  // namespace iocoro::ip

#pragma once

// IP-domain resolver.
//
// NOTE: Name/service resolution is inherently IP-specific, so it lives under `iocoro::ip`.

#include <iocoro/any_executor.hpp>
#include <iocoro/assert.hpp>
#include <iocoro/awaitable.hpp>
#include <iocoro/detail/unique_function.hpp>
#include <iocoro/error.hpp>
#include <iocoro/result.hpp>
#include <iocoro/thread_pool.hpp>

#include <atomic>
#include <cstring>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <system_error>
#include <vector>

#include <netdb.h>
#include <sys/socket.h>

namespace iocoro::ip {

/// Protocol-typed resolver facade for host/service -> `Protocol::endpoint` expansion.
///
/// IMPORTANT: `getaddrinfo()` is blocking. This resolver offloads it onto a `thread_pool`
/// executor (customizable) and resumes the awaiting coroutine on its original executor.
///
/// Cancellation is best-effort: a stop request prevents resumption with resolved results, but
/// cannot interrupt an in-flight `getaddrinfo()` call running on the pool.
/// A stop request observed before or during await suspension may still race with dispatch of the
/// blocking task, so internal pool work can still run even when the awaiter eventually observes
/// `operation_aborted`.
template <class Protocol>
class resolver {
 public:
  using protocol_type = Protocol;
  using endpoint = typename Protocol::endpoint;
  using results_type = std::vector<endpoint>;

  resolver() noexcept = default;
  explicit resolver(any_executor pool_ex) noexcept : pool_ex_(std::move(pool_ex)) {}

  resolver(resolver const&) = delete;
  auto operator=(resolver const&) -> resolver& = delete;
  resolver(resolver&&) noexcept = default;
  auto operator=(resolver&&) noexcept -> resolver& = default;

  /// Resolve `(host, service)` into a list of endpoints.
  ///
  /// - `host` may be a hostname or numeric address; empty is forwarded as nullptr to getaddrinfo.
  /// - `service` may be a service name or numeric port; empty is forwarded as nullptr.
  ///
  /// Returns `std::error_code` originating from `getaddrinfo()` on failure.
  ///
  /// NOTE: entries that cannot be converted into `Protocol::endpoint` are skipped silently.
  /// Successful conversion of any subset still yields success.
  auto async_resolve(std::string host, std::string service) -> awaitable<result<results_type>> {
    auto pool_ex = pool_ex_ ? *pool_ex_ : get_default_executor();
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

  // Shared state between the pool worker and the awaiting coroutine.
  //
  // SAFETY: this is shared-owned by both sides; resumption is posted onto `ex` to keep
  // coroutine resumption on the caller's executor.
  struct result_state {
    std::coroutine_handle<> continuation;
    any_executor ex;
    result<results_type> res{};
    std::atomic<bool> done{false};
    std::unique_ptr<std::stop_callback<::iocoro::detail::unique_function<void()>>> stop_cb{};
  };
  std::shared_ptr<result_state> state;

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

    if constexpr (requires { h.promise().get_stop_token(); }) {
      auto token = h.promise().get_stop_token();
      if (token.stop_requested()) {
        if (!st->done.exchange(true)) {
          st->res = unexpected(error::operation_aborted);
          st->ex.post([st]() { st->continuation.resume(); });
        }
      } else {
        // IMPORTANT: stop requests only affect the awaiting coroutine. The blocking
        // getaddrinfo() call cannot be interrupted; we only prevent delivering results.
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
              st->res = unexpected(error::operation_aborted);
              st->ex.post([st]() { st->continuation.resume(); });
            }});
      }
    }

    pool_ex.post([st, host_copy = std::move(host_copy), service_copy = std::move(service_copy)]() {
      result<results_type> res{};
      try {
        addrinfo hints;
        std::memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_UNSPEC;  // Accept both IPv4 and IPv6.
        hints.ai_socktype = Protocol::type();
        hints.ai_protocol = Protocol::protocol();

        addrinfo* result_list = nullptr;
        int const ret = ::getaddrinfo(host_copy.empty() ? nullptr : host_copy.c_str(),
                                      service_copy.empty() ? nullptr : service_copy.c_str(), &hints,
                                      &result_list);

        if (ret != 0) {
          res = unexpected(std::error_code(ret, addrinfo_error_category()));
        } else {
          results_type endpoints;
          for (auto* ai = result_list; ai != nullptr; ai = ai->ai_next) {
            auto ep_result = endpoint::from_native(ai->ai_addr, ai->ai_addrlen);
            if (ep_result) {
              endpoints.push_back(std::move(*ep_result));
            }
          }
          ::freeaddrinfo(result_list);
          res = std::move(endpoints);
        }
      } catch (...) {
        res = unexpected(error::internal_error);
      }

      if (st->done.exchange(true)) {
        return;
      }

      st->res = std::move(res);
      st->stop_cb.reset();
      st->ex.post([st]() { st->continuation.resume(); });
    });
  }

  auto await_resume() -> result<results_type> { return std::move(state->res); }
};

}  // namespace iocoro::ip

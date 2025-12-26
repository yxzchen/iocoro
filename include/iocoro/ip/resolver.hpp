#pragma once

// IP-domain resolver.
//
// Resolver is inherently IP-specific (host/service resolution), so it lives under `iocoro::ip`.

#include <iocoro/awaitable.hpp>
#include <iocoro/expected.hpp>
#include <iocoro/error.hpp>

#include <string>
#include <system_error>
#include <vector>

namespace iocoro::ip {

/// Protocol-typed resolver facade (minimal stub).
///
/// Responsibility boundary (locked-in):
/// - Accept host/service strings.
/// - Produce a list of `Protocol::endpoint` results.
/// - All protocol typing is via the `Protocol` template parameter.
///
/// Current status:
/// - Not implemented yet; returns `error::not_implemented`.
template <class Protocol>
class resolver {
 public:
  using protocol_type = Protocol;
  using endpoint = typename Protocol::endpoint;

  auto async_resolve(std::string /*host*/, std::string /*service*/)
    -> awaitable<expected<std::vector<endpoint>, std::error_code>> {
    co_return unexpected(std::error_code{iocoro::error::not_implemented});
  }
};

}  // namespace iocoro::ip




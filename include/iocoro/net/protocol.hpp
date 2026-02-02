#pragma once

#include <iocoro/result.hpp>

#include <concepts>
#include <system_error>

#include <sys/socket.h>

namespace iocoro::net {

/// Endpoint concept for sockaddr-based networking.
///
/// Semantics:
/// - Endpoint represents "address content + family" only.
/// - Endpoint MUST NOT influence socket type or protocol selection.
/// - `from_native()` is allowed to fail and should return an error (not UB).
template <class E>
concept endpoint_like =
  requires(E const& ep, sockaddr* out, socklen_t out_len, sockaddr const* in, socklen_t in_len) {
    // Native view of the endpoint.
    { ep.data() } -> std::convertible_to<sockaddr const*>;
    { ep.size() } -> std::convertible_to<socklen_t>;
    { ep.family() } -> std::convertible_to<int>;

    // Native conversions (symmetry with failure allowed).
    { E::from_native(in, in_len) } -> std::same_as<result<E>>;
    { ep.to_native(out, out_len) } -> std::same_as<result<socklen_t>>;
  };

/// Minimal protocol tag concept for sockaddr-based networking facades.
///
/// **Boundary rule (locked-in):**
/// - Protocol decides socket `type()` / `protocol()`.
/// - Endpoint decides `family()` + address content.
/// The dependency is one-way; an Endpoint must not "pick" socket type/protocol,
/// and a Protocol is not required to carry a family.
template <class P>
concept protocol_tag = requires {
  typename P::endpoint;
  requires endpoint_like<typename P::endpoint>;

  { P::type() } -> std::convertible_to<int>;
  { P::protocol() } -> std::convertible_to<int>;
};

}  // namespace iocoro::net

#pragma once

#include <concepts>
#include <type_traits>

namespace iocoro::ip {

/// Minimal protocol-tag concept used by iocoro's IP networking facades.
///
/// A Protocol tag should provide:
/// - `using endpoint = ip::basic_endpoint<Protocol>;`
/// - `static constexpr int type();`      // e.g. SOCK_STREAM
/// - `static constexpr int protocol();`  // e.g. IPPROTO_TCP
///
/// Note:
/// - We intentionally DO NOT require `family()` on the Protocol tag.
///   Family comes from `endpoint.family()` or user-specified open(family).
template <class P>
concept ip_protocol = requires {
  typename P::endpoint;
  { P::type() } -> std::convertible_to<int>;
  { P::protocol() } -> std::convertible_to<int>;
};

template <class P>
inline constexpr bool ip_protocol_v = ip_protocol<P>;

}  // namespace iocoro::ip



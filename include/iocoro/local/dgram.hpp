#pragma once

#include <iocoro/local/endpoint.hpp>
#include <iocoro/net/protocol.hpp>

#include <sys/socket.h>

namespace iocoro::local {

/// Local datagram protocol tag (AF_UNIX datagram sockets).
///
/// Note:
/// - We intentionally do NOT export a socket alias yet, because the net-level datagram facade
///   is not part of the current API surface.
struct dgram {
  using endpoint = local::endpoint;

  static constexpr auto type() noexcept -> int { return SOCK_DGRAM; }
  static constexpr auto protocol() noexcept -> int { return 0; }
};

static_assert(::iocoro::net::protocol_tag<dgram>);

}  // namespace iocoro::local

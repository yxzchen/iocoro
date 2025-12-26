#pragma once

#include <iocoro/ip/basic_endpoint.hpp>
#include <iocoro/ip/basic_resolver.hpp>
#include <iocoro/net/protocol.hpp>

#include <netinet/in.h>
#include <sys/socket.h>

namespace iocoro::ip {

/// UDP protocol tag (Asio-style).
///
/// Note: this step only defines the tag + endpoint alias; UDP socket/IO facades
/// are introduced when datagram semantics are implemented.
struct udp {
  using endpoint = ip::basic_endpoint<udp>;
  using resolver = ip::basic_resolver<udp>;

  static constexpr auto type() noexcept -> int { return SOCK_DGRAM; }
  static constexpr auto protocol() noexcept -> int { return IPPROTO_UDP; }
};

static_assert(::iocoro::net::protocol_tag<udp>);

}  // namespace iocoro::ip



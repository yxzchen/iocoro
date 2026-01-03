#pragma once

#include <iocoro/ip/endpoint.hpp>
#include <iocoro/ip/resolver.hpp>
#include <iocoro/net/basic_datagram_socket.hpp>
#include <iocoro/net/protocol.hpp>

#include <netinet/in.h>
#include <sys/socket.h>

namespace iocoro::ip {

/// UDP protocol tag (Asio-style).
///
/// Responsibilities:
/// - Provide endpoint alias.
/// - Provide socket type + protocol constants.
/// - Provide alias to datagram socket facade.
struct udp {
  using endpoint = ip::endpoint<udp>;
  using resolver = ip::resolver<udp>;
  using socket = ::iocoro::net::basic_datagram_socket<udp>;

  static constexpr auto type() noexcept -> int { return SOCK_DGRAM; }
  static constexpr auto protocol() noexcept -> int { return IPPROTO_UDP; }
};

static_assert(::iocoro::net::protocol_tag<udp>);

}  // namespace iocoro::ip

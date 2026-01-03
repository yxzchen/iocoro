#pragma once

#include <iocoro/local/endpoint.hpp>
#include <iocoro/net/basic_datagram_socket.hpp>
#include <iocoro/net/protocol.hpp>

#include <sys/socket.h>

namespace iocoro::local {

/// Local datagram protocol tag (AF_UNIX datagram sockets).
struct dgram {
  using endpoint = local::endpoint;
  using socket = ::iocoro::net::basic_datagram_socket<dgram>;

  static constexpr auto type() noexcept -> int { return SOCK_DGRAM; }
  static constexpr auto protocol() noexcept -> int { return 0; }
};

static_assert(::iocoro::net::protocol_tag<dgram>);

}  // namespace iocoro::local

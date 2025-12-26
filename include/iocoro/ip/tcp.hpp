#pragma once

#include <iocoro/ip/endpoint.hpp>
#include <iocoro/ip/resolver.hpp>
#include <iocoro/net/basic_acceptor.hpp>
#include <iocoro/net/basic_stream_socket.hpp>
#include <iocoro/net/protocol.hpp>

#include <netinet/in.h>
#include <sys/socket.h>

namespace iocoro::ip {

/// TCP protocol tag (Asio-style).
///
/// Responsibilities:
/// - Provide endpoint alias.
/// - Provide socket type + protocol constants.
/// - Provide aliases to higher-level networking facades (added in later steps).
struct tcp {
  using endpoint = ip::endpoint<tcp>;
  using acceptor = ::iocoro::net::basic_acceptor<tcp>;
  using resolver = ip::resolver<tcp>;
  using socket = ::iocoro::net::basic_stream_socket<tcp>;

  static constexpr auto type() noexcept -> int { return SOCK_STREAM; }
  static constexpr auto protocol() noexcept -> int { return IPPROTO_TCP; }
};

static_assert(::iocoro::net::protocol_tag<tcp>);

}  // namespace iocoro::ip

#pragma once

#include <iocoro/ip/basic_endpoint.hpp>
#include <iocoro/ip/basic_acceptor.hpp>
#include <iocoro/ip/basic_resolver.hpp>
#include <iocoro/ip/basic_stream_socket.hpp>
#include <iocoro/ip/protocol_concepts.hpp>

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
  using endpoint = ip::basic_endpoint<tcp>;
  using acceptor = ip::basic_acceptor<tcp>;
  using resolver = ip::basic_resolver<tcp>;
  using socket = ip::basic_stream_socket<tcp>;

  static constexpr auto type() noexcept -> int { return SOCK_STREAM; }
  static constexpr auto protocol() noexcept -> int { return IPPROTO_TCP; }
};

static_assert(ip_protocol<tcp>);

}  // namespace iocoro::ip



#pragma once

#include <iocoro/ip/basic_endpoint.hpp>
#include <iocoro/ip/protocol.hpp>

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

  static constexpr auto type() noexcept -> int { return SOCK_STREAM; }
  static constexpr auto protocol() noexcept -> int { return IPPROTO_TCP; }
};

static_assert(ip_protocol<tcp>);

}  // namespace iocoro::ip



#pragma once

#include <iocoro/local/endpoint.hpp>

#include <iocoro/net/basic_acceptor.hpp>
#include <iocoro/net/basic_stream_socket.hpp>
#include <iocoro/net/protocol.hpp>

#include <sys/socket.h>

namespace iocoro::local {

/// Local stream protocol tag (AF_UNIX stream sockets).
struct stream {
  using endpoint = local::endpoint;

  static constexpr auto type() noexcept -> int { return SOCK_STREAM; }
  static constexpr auto protocol() noexcept -> int { return 0; }

  using socket = ::iocoro::net::basic_stream_socket<stream>;
  using acceptor = ::iocoro::net::basic_acceptor<stream>;
};

static_assert(::iocoro::net::protocol_tag<stream>);

}  // namespace iocoro::local

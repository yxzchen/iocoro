#pragma once

// Aggregated public header for IP-related networking components.
// Provides address types, resolvers, and TCP/UDP sockets, endpoints,
// and acceptors. Most users should include <iocoro/iocoro.hpp>.

#include <iocoro/ip/address.hpp>
#include <iocoro/ip/basic_endpoint.hpp>
#include <iocoro/ip/resolver.hpp>

#include <iocoro/ip/tcp/acceptor.hpp>
#include <iocoro/ip/tcp/endpoint.hpp>
#include <iocoro/ip/tcp/socket.hpp>

#include <iocoro/ip/udp/endpoint.hpp>
#include <iocoro/ip/udp/socket.hpp>

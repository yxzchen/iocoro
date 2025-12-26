#pragma once

// Aggregated public header for IP-related networking components.
// Provides address types, resolvers, and TCP/UDP sockets, endpoints,
// and acceptors. Most users should include <iocoro/iocoro.hpp>.

#include <iocoro/ip/address.hpp>
#include <iocoro/ip/endpoint.hpp>
#include <iocoro/ip/resolver.hpp>

#include <iocoro/ip/tcp.hpp>
#include <iocoro/ip/udp.hpp>

#include <iocoro/impl/ip/address.ipp>
#include <iocoro/impl/ip/endpoint_storage.ipp>

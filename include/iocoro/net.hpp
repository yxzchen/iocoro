#pragma once

// Aggregated public header for protocol-agnostic networking facades.
//
// This header provides the "socket abstraction layer" (facades) that is not specific to
// any particular address family domain (IP vs local/AF_UNIX, etc.).

#include <iocoro/net/protocol.hpp>

#include <iocoro/net/basic_acceptor.hpp>
#include <iocoro/net/basic_stream_socket.hpp>

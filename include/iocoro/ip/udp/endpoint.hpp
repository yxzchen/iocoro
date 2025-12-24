#pragma once

#include <iocoro/ip/endpoint.hpp>

namespace iocoro::ip::udp {

/// UDP endpoint type.
///
/// Currently this is an alias of the generic IP endpoint representation.
using endpoint = ::iocoro::ip::endpoint;

}  // namespace iocoro::ip::udp

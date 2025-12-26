#pragma once

// Aggregated public header for local (AF_UNIX) domain networking components.
//
// This domain is named `local` (not `unix`) because AF_UNIX is not Unix-OS specific.

#include <iocoro/local/dgram.hpp>
#include <iocoro/local/endpoint.hpp>
#include <iocoro/local/stream.hpp>

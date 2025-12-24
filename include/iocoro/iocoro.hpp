#pragma once

// Primary public header for the iocoro coroutine-based I/O library.
// Most users should include this header only.

// Error & result model
#include <iocoro/error.hpp>
#include <iocoro/expected.hpp>

// Coroutine & completion model
#include <iocoro/awaitable.hpp>
#include <iocoro/completion_token.hpp>
#include <iocoro/this_coro.hpp>

#include <iocoro/co_sleep.hpp>
#include <iocoro/co_spawn.hpp>

// Execution & lifetime
#include <iocoro/executor.hpp>
#include <iocoro/io_context.hpp>
#include <iocoro/work_guard.hpp>

// Timers & composition
#include <iocoro/steady_timer.hpp>
#include <iocoro/timer_handle.hpp>

#include <iocoro/when_all.hpp>
#include <iocoro/when_any.hpp>

// Networking
#include <iocoro/basic_socket.hpp>
#include <iocoro/ip.hpp>

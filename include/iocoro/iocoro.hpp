#pragma once

// Primary public header for the iocoro coroutine-based I/O library.
// Most users should include this header only.

// Error & result model
#include <iocoro/error.hpp>
#include <iocoro/expected.hpp>

// Coroutine & completion model
#include <iocoro/awaitable.hpp>
#include <iocoro/bind_executor.hpp>
#include <iocoro/completion_token.hpp>
#include <iocoro/this_coro.hpp>

#include <iocoro/co_sleep.hpp>
#include <iocoro/co_spawn.hpp>

// Execution & lifetime
#include <iocoro/io_context.hpp>
#include <iocoro/io_executor.hpp>
#include <iocoro/thread_pool.hpp>
#include <iocoro/work_guard.hpp>

// Timers & composition
#include <iocoro/steady_timer.hpp>

#include <iocoro/when_all.hpp>
#include <iocoro/when_any.hpp>

// Networking
#include <iocoro/net/basic_acceptor.hpp>
#include <iocoro/net/basic_stream_socket.hpp>
#include <iocoro/net/protocol.hpp>

// Async I/O algorithms
#include <iocoro/io/async_read.hpp>
#include <iocoro/io/async_read_until.hpp>
#include <iocoro/io/async_write.hpp>
#include <iocoro/io/stream_concepts.hpp>
#include <iocoro/io/with_timeout.hpp>

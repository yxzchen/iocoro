#pragma once

// Primary public header for the iocoro coroutine-based I/O library.
// Most users should include this header only.

// Error & result model
#include <iocoro/error.hpp>
#include <iocoro/expected.hpp>
#include <iocoro/result.hpp>

// Coroutine & completion model
#include <iocoro/any_executor.hpp>
#include <iocoro/any_io_executor.hpp>
#include <iocoro/awaitable.hpp>
#include <iocoro/bind_executor.hpp>
#include <iocoro/co_spawn.hpp>
#include <iocoro/completion_token.hpp>
#include <iocoro/this_coro.hpp>

// Execution & lifetime
#include <iocoro/io_context.hpp>
#include <iocoro/strand.hpp>
#include <iocoro/thread_pool.hpp>
#include <iocoro/work_guard.hpp>

// Timers & composition
#include <iocoro/awaitable_operators.hpp>
#include <iocoro/co_sleep.hpp>
#include <iocoro/condition_event.hpp>
#include <iocoro/steady_timer.hpp>
#include <iocoro/with_timeout.hpp>
#include <iocoro/when_all.hpp>
#include <iocoro/when_any.hpp>

// Networking
#include <iocoro/net/basic_acceptor.hpp>
#include <iocoro/net/basic_stream_socket.hpp>
#include <iocoro/net/buffer.hpp>
#include <iocoro/net/protocol.hpp>
#include <iocoro/shutdown.hpp>
#include <iocoro/socket_option.hpp>

// Async I/O algorithms
#include <iocoro/io/read.hpp>
#include <iocoro/io/read_until.hpp>
#include <iocoro/io/stream_concepts.hpp>
#include <iocoro/io/write.hpp>

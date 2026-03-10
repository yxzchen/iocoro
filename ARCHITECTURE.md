# iocoro Architecture

## System Picture

```text
+--------------------------------------------------------------+
|                     User Coroutines / App Code               |
|         awaitable workflows, timers, sockets, resolver       |
+-------------------------------+------------------------------+
                                |
                                v
+--------------------------------------------------------------+
|                        Public API Layer                      |
|  awaitable | co_spawn | io_context | steady_timer | socket   |
|  resolver  | when_all | when_any   | with_timeout | buffers  |
+-------------------------------+------------------------------+
                                |
                                v
+--------------------------------------------------------------+
|                         Runtime Layer                        |
|  any_executor / any_io_executor                              |
|  coroutine promise + continuation                            |
|  cancellation via stop_token                                 |
|  strand_executor / thread_pool                               |
+-------------------------------+------------------------------+
                                |
                                v
+--------------------------------------------------------------+
|                       Event Loop Core                        |
|  detail::io_context_impl                                     |
|  posted_queue | timer_registry | fd_registry                 |
+-------------------------------+------------------------------+
                                |
                                v
+--------------------------------------------------------------+
|                        Linux Backend                         |
|                 epoll (default) | io_uring                   |
+--------------------------------------------------------------+
```

## Repository Layout

```text
/include/iocoro/          public headers and user-facing API
/include/iocoro/detail/   internal runtime and low-level building blocks
/include/iocoro/impl/     inline .ipp implementations and backend code
/examples/                small runnable samples
/benchmark/               benchmark cases, configs, scripts, and reports
/test/                    behavior and semantic tests
```

Important packaging note:

```text
CMake target: iocoro
Type:         INTERFACE library
Meaning:      header-only, no compiled static/shared library artifact
```

## Runtime Center

```text
io_context
   |
   +-- io_context::executor_type
   |
   +-- detail::io_context_impl
           |
           +-- posted_queue     -> cross-thread posted callbacks
           +-- timer_registry   -> timer waiters and expiry ordering
           +-- fd_registry      -> read/write readiness waiters
           +-- backend_interface
                   |
                   +-- epoll backend
                   +-- io_uring backend
```

`io_context` is the center of the runtime.
It owns the event loop that drives:

- posted tasks
- timers
- fd readiness events

## Coroutine and Execution Model

```text
co_spawn(...)
   |
   v
awaitable<T>
   |
   v
awaitable_promise
   |
   +-- bound executor
   +-- continuation handle
   +-- exception storage
   +-- stop_token / stop_source
   |
   v
resume on executor
```

Main idea:

- coroutines are bound to executors, not directly to threads
- `co_spawn(...)` starts a coroutine by posting its first resume onto an executor
- the executor determines where continuation work runs

## Executors

```text
any_executor
   |
   +-- io_context::executor_type
   +-- thread_pool::executor_type
   +-- strand_executor

any_io_executor
   |
   +-- wraps only IO-capable executors
   +-- currently requires an io_context-backed executor
```

Role of each executor:

- `io_context::executor_type`
  event-loop executor with I/O capability
- `thread_pool::executor_type`
  worker executor for blocking or non-reactor work
- `strand_executor`
  serializes tasks on top of another executor

## Event Loop Execution Order

```text
run() / run_for()
        |
        +--> drain posted_queue
        +--> process expired timers
        +--> wait for backend events
        +--> dispatch ready fd operations

run_one()
        |
        +--> process one scheduler turn
        +--> return after the first stage that makes progress
```

This loop handles three categories of work:

- posted callbacks
- timer completions
- socket/fd readiness completions

Important invariant:

```text
One io_context
    -> at most one active event-loop thread
    -> serialized reactor-owned state
```

## Reactor Model

```text
async operation
    |
    +--> register interest in io_context_impl
            |
            +--> timer_registry   (for timers)
            +--> fd_registry      (for fd readiness)
                    |
                    v
              Linux backend waits
                    |
                    v
               ready event arrives
                    |
                    v
             coroutine is resumed
```

This is a Reactor design, not a Proactor design:

- the library waits for readiness
- the coroutine resumes after readiness
- the actual syscall path continues after resumption

Important current note:

```text
epoll backend    -> readiness-based
io_uring backend -> still exposed as readiness-based to upper layers
```

## Networking Layering

```text
Public networking API
    |
    +-- basic_stream_socket<Protocol>
    +-- basic_datagram_socket<Protocol>
    +-- basic_acceptor<Protocol>
    |
    v
Semantic layer
    |
    +-- protocol tags
    +-- endpoint types
    +-- user-visible connect/bind/listen/accept/read/write
    |
    v
Handle layer
    |
    +-- socket_handle_base
    |
    v
Concrete socket implementations
    |
    +-- stream_socket_impl
    +-- datagram_socket_impl
    +-- acceptor_impl
    |
    v
FD lifecycle core
    |
    +-- socket_impl_base
    |
    v
io_context reactor
```

`resolver<Protocol>` is public networking-facing API, but it does not follow the socket/fd/reactor
path above. It performs blocking `getaddrinfo()` on a `thread_pool` executor and resumes on the
original executor.

Design split:

- upper layers describe networking semantics
- lower layers manage fd ownership, readiness waiting, cancel/close behavior

## Typical Async Read Path

```text
User coroutine
    |
    +--> socket.async_read_some()
            |
            +--> try non-blocking read immediately
            |
            +--> if would block:
                    register read wait in io_context
                        |
                        +--> fd_registry stores waiter
                        +--> backend waits for readability
                        +--> ready event arrives
                        +--> waiter is resumed
                        +--> coroutine continues
                        +--> read path finishes and returns result
```

Core pattern:

```text
suspend -> register -> wait -> wake -> resume
```

Timers follow the same shape, but go through `timer_registry` instead of `fd_registry`.

## Blocking Work Path

```text
Coroutine
   |
   +--> resolver::async_resolve(...)
           |
           +--> thread_pool
                   |
                   +--> blocking getaddrinfo()
                   |
                   +--> post result back to original executor
```

Why this exists:

- not all async-looking work belongs in the Reactor
- DNS resolution is the clearest example because `getaddrinfo()` is blocking

## Cancellation Path

```text
stop_token / request_stop()
        |
        v
operation_awaiter
        |
        v
event_handle.cancel()
        |
        v
timer_registry / fd_registry
        |
        v
operation completes with aborted/error
```

Cancellation in this project is:

- cooperative
- best-effort
- state-driven rather than preemptive

## Concurrency Model

```text
Foreign threads
    |
    +-- post(...)
    +-- stop()
    +-- cancel()
    |
    v
io_context entry points
    |
    v
single active reactor thread
    |
    v
reactor-owned state mutation
```

The strategy is intentionally conservative:

- reactor state is serialized on one event-loop thread
- external threads can still safely post work or request stop/cancel
- parallel worker execution is provided separately by `thread_pool`

## Main Design Choices

```text
iocoro
  |
  +-- header-only packaging
  +-- C++20 coroutine-based API
  +-- executor-bound coroutine scheduling
  +-- single-reactor io_context runtime
  +-- readiness-based I/O model
  +-- protocol-typed networking facade
  +-- cooperative cancellation
```

What stands out architecturally:

- one consistent runtime model across timers, sockets, and coroutine composition
- clear separation between public API, runtime core, and Linux backend
- readiness abstraction preserved even when backend implementation changes
- explicit boundaries instead of over-complicated concurrency

## Current Architectural Boundaries

```text
Scope today
  |
  +-- Linux only
  +-- header-only
  +-- one active event-loop thread per io_context
  +-- readiness-based I/O
  +-- controlled socket operation concurrency
  +-- cooperative, not preemptive, cancellation
```

These are part of the design scope, not accidental omissions.

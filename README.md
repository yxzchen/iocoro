# iocoro

`iocoro` is a **development-stage (Development / Experimental)** coroutine-based I/O library for C++20.

### Important notice

- This project **does not guarantee semantic correctness**.
- This project **does not guarantee API stability**; APIs and behavior may change frequently.
- This project **does not promise backward compatibility** and is **not recommended for production use**.

## Goals and scope

- **What it tries to solve**: provide an executor/awaitable-centric playground for coroutine-based async I/O and composition, suitable for experimentation and discussion.
- **Target users**: experienced C++ developers who are already comfortable with C++20 coroutines and executor/scheduling semantics, and who can tolerate frequent changes.

## Key capabilities (semantic-level)

- **`awaitable<T>`** as a coroutine return type and composition unit.
- **Executor abstraction** via type-erased executors (e.g. `any_executor`, `any_io_executor`) to carry scheduling semantics.
- **Spawning and completion model**: `co_spawn` supports `detached`, `use_awaitable`, and completion callbacks.
- **Stop/cancellation propagation (via `std::stop_token`)**: promise owns a stop token and supports requesting stop (details evolve with the project).
- **Core I/O building blocks**: `io_context`, timers, sockets, and basic async algorithms (development-stage semantics).

## Minimal example

This is the smallest runnable shape: create an `io_context`, spawn a coroutine, and run the event loop.

```cpp
#include <iocoro/iocoro.hpp>

#include <chrono>
#include <iostream>

using namespace std::chrono_literals;

auto main_task() -> iocoro::awaitable<void> {
  std::cout << "hello from iocoro\n";
  co_await iocoro::co_sleep(50ms);
  std::cout << "done\n";
}

int main() {
  iocoro::io_context ctx;

  iocoro::co_spawn(ctx.get_executor(), main_task(), iocoro::detached);
  ctx.run();
  return 0;
}
```

## Networking support (development stage)

Networking support is still evolving; exact APIs and semantics may change.

- **IP layer**
  - IPv4/IPv6 addresses and typed endpoints.
  - TCP: `iocoro::ip::tcp::{acceptor,socket}`.
  - UDP: `iocoro::ip::udp::{socket}`.
  - Resolver: `iocoro::ip::{tcp,udp}::resolver`.

- **Local (AF_UNIX) layer**
  - Local stream and datagram endpoints and sockets under `iocoro::local::*`.

- **Async I/O algorithms**
  - Stream-oriented helpers such as `iocoro::io::async_read`, `async_write`, `async_read_until` for supported stream types.

- **Backend notes**
  - Linux uses an epoll-based backend by default.
  - An io_uring backend is optional when `liburing` is available and enabled via defining `IOCORO_ENABLE_URING`.

## Quick start

Examples may change as the implementation evolves.

### Requirements

- CMake \(>= 3.15\)
- A C++20 compiler with coroutine support (g++ / clang++)

### Build and run examples (minimal path)

Tests are **OFF by default**. This keeps the minimal path free of GTest requirements.

```bash
cmake -S . -B build -DIOCORO_BUILD_EXAMPLES=ON -DIOCORO_BUILD_TESTS=OFF
cmake --build build -j
./build/examples/hello_io_context
```

### Minimal TCP example (recommended end-to-end demo)

Run the single-process echo demo:

```bash
./build/examples/tcp_echo
```

It starts an acceptor bound to `127.0.0.1:0` (ephemeral port), then connects a client to the acceptor's `local_endpoint()`, writes a line, reads the echoed line, and exits.

### Developer self-check (build and run tests)

Tests require **GTest**. Enable tests explicitly:

```bash
./build.sh -t
```

or:

```bash
cmake -S . -B build -DIOCORO_BUILD_TESTS=ON
cmake --build build -j
ctest --test-dir build --output-on-failure -j
```

## Examples guide

`examples/` is the most important documentation surface during development stage. Each example tries to focus on one topic and only uses public APIs.

- `examples/hello_io_context.cpp`
  - **Goal**: minimal `io_context` + `co_spawn` + `co_sleep` workflow.
  - **Preconditions**: requires an IO-capable executor (from `io_context`).
  - **Assumptions**: demonstrates usage only; no stable semantic promises.

- `examples/tcp_echo.cpp`
  - **Goal**: minimal TCP accept/connect + read/write in a single process.
  - **Preconditions**: loopback networking on the host.
  - **Assumptions**: protocol and error semantics may change.

- `examples/switch_executor.cpp`
  - **Goal**: demonstrate `this_coro::switch_to()` across executors.
  - **Preconditions**: after switching to a non-IO executor, do not call IO-only operations until switching back.
  - **Assumptions**: switching semantics are development-stage and may evolve.

## License

MIT License. See `LICENSE`.


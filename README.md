# iocoro

`iocoro` is a **development-stage** coroutine-based I/O library for C++20.

It aims to provide an executor/awaitable-centric playground for coroutine-based async I/O and composition, suitable for experimentation and discussion. It targets experienced C++ developers who are already comfortable with C++20 coroutines and executor/scheduling semantics, and can tolerate frequent changes.

### Important notice

- This project **does not guarantee semantic correctness**.
- This project **does not guarantee API stability**; APIs and behavior may change frequently.
- This project **does not promise backward compatibility** and is **not recommended for production use**.

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

## Networking support

Networking is experimental; APIs and semantics may change.

- IP: TCP/UDP sockets and typed endpoints under `iocoro::ip::*` (plus resolvers).
- Local (AF_UNIX): local stream/datagram under `iocoro::local::*`.
- Composed I/O: `iocoro::io::{async_read, async_write, async_read_until}` for supported stream types.
- Linux backend: epoll by default; optional io_uring when `liburing` is available and `IOCORO_ENABLE_URING=ON`.

## Quick start

Requirements: CMake \(>= 3.15\) and a C++20 compiler (g++ / clang++).

Build (Debug by default, tests OFF by default):

```bash
./build.sh
```

Run the minimal TCP echo example (server + client):

```bash
./build/examples/tcp_echo_server
./build/examples/tcp_echo_client
```

Run tests (requires GTest):

```bash
./build.sh -t
```

Build benchmarks:

```bash
./build.sh -b
```

## License

MIT License. See `LICENSE`.

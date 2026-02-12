# iocoro

`iocoro` is a **0.x preview** coroutine-based I/O library for C++20.

It aims to provide an executor/awaitable-centric playground for coroutine-based async I/O and composition, suitable for experimentation and discussion. It targets experienced C++ developers who are already comfortable with C++20 coroutines and executor/scheduling semantics, and can tolerate frequent API/behavior changes.

### Status & stability

- This is a **preview/testing release** (0.x): APIs and behavior may change frequently.
- This project **does not promise backward compatibility** during 0.x.
- CI tries to cover correctness, sanitizers, and installability, but you should treat it as **experimental**.

## Key capabilities (semantic-level)

- **`awaitable<T>`** as a coroutine return type and composition unit.
- **Executor abstraction** via type-erased executors (e.g. `any_executor`, `any_io_executor`) to carry scheduling semantics.
- **Spawning and completion model**: `co_spawn` supports `detached`, `use_awaitable`, and completion callbacks.
- **Stop/cancellation propagation (via `std::stop_token`)**: promise owns a stop token and supports requesting stop (details evolve with the project).
- **Core I/O building blocks**: `io_context`, timers, sockets, and basic async algorithms (development-stage semantics).

## Platform & toolchain support

- **OS**: Linux
- **C++**: C++20 (coroutines required)
- **Build**: CMake \(>= 3.15\)
- **Compilers**: GCC / Clang with C++20 coroutine support (CI covers GCC/Clang on Ubuntu)
- **Backends**:
  - **epoll**: default
  - **io_uring**: **opt-in**, requires `liburing` and a compatible kernel (typically 5.1+)

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
- Linux backend: epoll by default; **io_uring is opt-in**.

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

## Install and use with CMake (`find_package`)

Install to a prefix:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$HOME/.local"
cmake --build build -j
cmake --install build
```

Consume from another project:

```cmake
find_package(iocoro REQUIRED)
target_link_libraries(your_target PRIVATE iocoro::iocoro)
```

### Backend selection (io_uring opt-in)

By default, `iocoro::iocoro` uses **epoll**.

If you want **io_uring**, enable it explicitly before `find_package(iocoro)`:

```cmake
set(IOCORO_ENABLE_URING ON CACHE BOOL "Enable iocoro io_uring backend")
find_package(iocoro REQUIRED)
```

If `IOCORO_ENABLE_URING=ON` but `liburing` is not found, CMake will keep using epoll.

Build benchmarks:

```bash
./build.sh -b
```

### Performance baseline, gate, and observability

Run TCP roundtrip benchmark matrix:

```bash
./benchmark/run_tcp_roundtrip_baseline.sh \
  --build-dir build \
  --run-timeout-sec 120 \
  --baseline benchmark/baseline/tcp_roundtrip_thresholds.txt \
  --report benchmark/perf_report.json
```

Run TCP connect/accept benchmark matrix:

```bash
./benchmark/run_tcp_connect_accept_baseline.sh \
  --build-dir build \
  --run-timeout-sec 180 \
  --baseline benchmark/baseline/tcp_connect_accept_thresholds.txt \
  --report benchmark/connect_accept_report.json
```

Validate report schemas:

```bash
python3 benchmark/validate_perf_report.py \
  --schema benchmark/perf_report.schema.json \
  --report benchmark/perf_report.json
```

```bash
python3 benchmark/validate_perf_report.py \
  --schema benchmark/connect_accept_report.schema.json \
  --report benchmark/connect_accept_report.json
```

## License

MIT License. See `LICENSE`.

# iocoro

`iocoro` is a C++20 coroutine-based async I/O library focused on executor/awaitable composition and network I/O.

## Status

- `0.x` preview: APIs and behavior can change.
- No backward-compatibility promise during `0.x`.

## Requirements

- Linux
- CMake `>= 3.15`
- GCC/Clang with C++20 coroutine support
- `pthread`

## Minimal Example

```cpp
#include <iocoro/iocoro.hpp>

#include <chrono>
#include <iostream>

using namespace std::chrono_literals;

auto hello() -> iocoro::awaitable<void> {
  std::cout << "start\n";
  co_await iocoro::co_sleep(50ms);
  std::cout << "done\n";
}

int main() {
  iocoro::io_context ctx;
  iocoro::co_spawn(ctx.get_executor(), hello(), iocoro::detached);
  ctx.run();
}
```

## Build

```bash
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DIOCORO_BUILD_EXAMPLES=ON \
  -DIOCORO_BUILD_TESTS=OFF \
  -DIOCORO_BUILD_BENCHMARKS=OFF
cmake --build build -j
```

Optional backend:

- `-DIOCORO_ENABLE_URING=ON` to enable io_uring when `liburing` is available.

## Install

```bash
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$HOME/.local"
cmake --build build -j
cmake --install build
```

## Use in CMake

```cmake
find_package(iocoro REQUIRED)
target_link_libraries(your_target PRIVATE iocoro::iocoro)
```

## Examples

Build with `-DIOCORO_BUILD_EXAMPLES=ON`, then run:

```bash
./build/examples/hello_io_context
./build/examples/tcp_echo_server
./build/examples/tcp_echo_client
```

## More Docs

- Contributing and developer workflows: `CONTRIBUTING.md`
- Benchmark usage and reports: `benchmark/README.md`

## License

MIT. See `LICENSE`.

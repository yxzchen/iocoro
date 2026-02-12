# iocoro

`iocoro` is a C++20 coroutine-based async I/O library focused on executor/awaitable composition and network I/O.

## Status

- `0.x` preview: APIs and behavior can change.
- No backward-compatibility promise during `0.x`.
- Intended for advanced users who are comfortable with coroutine/executor semantics.

## Platform and toolchain

- OS: Linux
- Language: C++20
- Build: CMake `>= 3.15`
- Compilers: GCC/Clang with coroutine support
- Runtime backend: epoll (default)

## Build

### Quick build script

```bash
./build.sh
```

Useful flags:

- `./build.sh -r` for Release
- `./build.sh -t` to enable and run tests
- `./build.sh -b` to build benchmarks (`-DIOCORO_BUILD_BENCHMARKS=ON`)

### Raw CMake

```bash
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DIOCORO_BUILD_EXAMPLES=ON \
  -DIOCORO_BUILD_TESTS=ON \
  -DIOCORO_BUILD_BENCHMARKS=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
```

## Examples

Build examples:

```bash
cmake -S . -B build -DIOCORO_BUILD_EXAMPLES=ON
cmake --build build -j
```

Run a few:

```bash
./build/examples/hello_io_context
./build/examples/tcp_echo_server
./build/examples/tcp_echo_client
```

## Performance benchmark entrypoint

Use a single unified script:

```bash
./benchmark/scripts/run_all_perf_benchmarks.sh \
  --build-dir build \
  --iterations 3 \
  --warmup 1 \
  --timeout-sec 180
```

This command runs all benchmark suites:

- tcp roundtrip
- tcp latency
- tcp connect/accept
- tcp throughput
- udp send/receive
- timer churn
- thread pool scaling

Default behavior:

- Runs iocoro vs Asio comparison for all suites.
- Applies baseline regression gate.
- Validates generated JSON reports against schemas.

For benchmark details, see `benchmark/README.md`.

## Install and consume

Install:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$HOME/.local"
cmake --build build -j
cmake --install build
```

Consume:

```cmake
find_package(iocoro REQUIRED)
target_link_libraries(your_target PRIVATE iocoro::iocoro)
```

## License

MIT. See `LICENSE`.

# ioxz

**Prototype async I/O library for modern C++**

ioxz is a header-only asynchronous I/O library built for [redisxz](https://github.com/xyz/redisxz), a modern Redis client. It provides a clean, coroutine-based API for async operations with support for multiple backends.

## Features

- **C++20 Coroutines**: Native async/await support
- **Dual Backend**: io_uring (preferred) or epoll fallback
- **Header-Only**: Easy integration, no linking required
- **Type-Safe Error Handling**: Uses `expected<T, E>` pattern
- **Zero-Copy**: Efficient buffer management with `std::span`
- **Work Guard**: RAII `work_guard` class keeps event loop running

## Work Guard

The `work_guard<T>` template class prevents executor `T`'s `run()` from exiting when there are no pending operations. It uses RAII for automatic lifecycle management:

```cpp
{
    work_guard<io_context> guard(ctx);  // Adds work guard
    // ... do async work ...
}  // Automatically removes work guard when guard is destroyed
```

The template accepts any executor type that has `add_work_guard()` and `remove_work_guard()` methods.

## Build Requirements

- C++20 compiler (GCC 10+, Clang 12+, MSVC 19.28+)
- CMake 3.20+
- fmt library
- liburing (optional, for io_uring support)
- Google Test (for building tests)

## Quick Example

```cpp
#include <xz/io/io.hpp>
#include <xz/io/ip.hpp>
#include <xz/io/task.hpp>

using namespace xz::io;

auto async_example() -> task<void> {
    io_context ctx;
    tcp_socket socket(ctx);

    // Use RAII work_guard to keep event loop running
    work_guard<io_context> guard(ctx);

    // Connect to Redis
    auto endpoint = ip::tcp_endpoint{
        ip::address_v4{{127, 0, 0, 1}}, 6379
    };
    co_await socket.async_connect(endpoint, 1000ms);

    // Send PING command
    std::string cmd = "*1\r\n$4\r\nPING\r\n";
    co_await socket.async_write_some(std::span<const char>{cmd.data(), cmd.size()});

    // Read response
    char buffer[256];
    auto bytes_read = co_await socket.async_read_some(std::span<char>{buffer, sizeof(buffer)});
    std::cout << "Response: " << std::string(buffer, bytes_read) << std::endl;

    // Work guard automatically removed when guard goes out of scope
}

int main() {
    io_context ctx;
    auto task = async_example();
    task.resume();
    ctx.run();  // Will keep running until work guard is removed
    return 0;
}
```

## Building

```bash
cmake -B build -DIOXZ_BUILD_TESTS=OFF
cmake --build build
```

## Status

**This is a prototype library** developed specifically for redisxz. While functional, it is not intended for production use in other projects without thorough testing.

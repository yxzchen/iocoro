# ioxz

A header-only C++20 coroutine-based async I/O library. This project is under development, and its API is not yet considered stable.

**Status**: This is a prototype library developed specifically for [redisxz](https://github.com/yxzchen/redisxz), a modern Redis client.

## Overview

ioxz provides a minimal, modern interface for asynchronous network I/O using C++20 coroutines. It supports io_uring on Linux (with epoll fallback) and aims to provide zero-overhead abstractions for building high-performance async applications.

## Quick Example

```cpp
#include <xz/io.hpp>
#include <xz/io/src.hpp>

#include <chrono>
#include <iostream>

using namespace std::chrono_literals;
using namespace xz::io;

auto example(io_context& ctx) -> task<void> {
  tcp_socket socket(ctx);

  // Connect
  auto endpoint = ip::tcp_endpoint{ip::address_v4::from_string("127.0.0.1"), 6379};
  co_await socket.async_connect(endpoint, 1000ms);

  // Write
  std::string data = "PING\r\n";
  co_await socket.async_write_some(std::span{data.data(), data.size()});

  // Read
  char buffer[256];
  auto n = co_await socket.async_read_some(std::span{buffer, 256});

  std::cout << "Response: " << std::string_view(buffer, n) << std::endl;
}

int main() {
  io_context ctx;
  co_spawn(ctx, example(ctx), use_detached);
  ctx.run();
}
```

## Features

### Core Components
- **`io_context`** - Event loop with io_uring/epoll backend
- **`task<T>`** - Lazy coroutine type with symmetric transfer
- **`co_spawn`** - Launch coroutines on an executor (detached mode)
- **`awaitable_op<T>`** - Base awaitable type for async operations

### Networking
- **`tcp_socket`** - Asynchronous TCP socket
  - `async_connect()` - Connect with timeout
  - `async_read_some()` - Read with timeout
  - `async_write_some()` - Write with timeout
  - Socket options: TCP_NODELAY, SO_KEEPALIVE

- **`ip::address_v4`** - IPv4 addresses
- **`ip::address_v6`** - IPv6 addresses (basic support)
- **`ip::tcp_endpoint`** - TCP endpoint (address + port)

### Utilities
- **`expected<T, E>`** - Result type for error handling
- **`work_guard<T>`** - RAII guard to keep event loop running
- **Timers** - Schedule callbacks with millisecond precision

## Missing Features

This is a minimal library focused on redisxz requirements. Notable omissions:

- **No UDP support**
- **No SSL/TLS** - Planned for redisxz
- **No acceptor/server sockets** - Client-only
- **No scatter-gather I/O**
- **No file I/O operations**
- **No async DNS resolution**
- **No completion tokens** beyond `use_detached`
- **No cancellation** - Can only close sockets
- **No strand/serialization executor**
- **Limited IPv6 support** - Basic structures only

## Build Requirements

- **Platform**: Linux only
- **Compiler**: GCC 10+ (Clang/MSVC not tested)
- **C++ Standard**: C++20 (for coroutines)
- **CMake**: 3.15 or higher
- **Kernel**: Linux 5.1+ for io_uring support (5.4+ recommended), falls back to epoll on older kernels
- **Dependencies**:
  - [fmt](https://github.com/fmtlib/fmt) - Formatting library
  - [liburing](https://github.com/axboe/liburing) - Optional, for io_uring support
  - [Google Test](https://github.com/google/googletest) - Optional, for tests

## Building

```bash
# Header-only - no build needed
# Just add include/ to your include path

# Build tests
cmake -B build -DIOXZ_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build
```

## Integration

### Using CMake FetchContent (Recommended)

```cmake
include(FetchContent)

FetchContent_Declare(
    ioxz
    GIT_REPOSITORY https://github.com/yxzchen/ioxz.git
    GIT_TAG master
)
FetchContent_MakeAvailable(ioxz)

target_link_libraries(your_target PRIVATE ioxz)
```

### Using Git Submodule

```bash
git submodule add https://github.com/yxzchen/ioxz.git third_party/ioxz
```

```cmake
add_subdirectory(third_party/ioxz)
target_link_libraries(your_target PRIVATE ioxz)
```

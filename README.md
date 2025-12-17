# ioxz

A header-only C++20 coroutine-based async I/O library.

This is a prototype library developed specifically for [redisxz](https://github.com/yxzchen/redisxz), a modern Redis client.

This project is under development, and its API is not yet considered stable.

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

auto example(io_context& ctx) -> awaitable<void> {
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
  
  // Spawn with completion handler for error reporting
  co_spawn(ctx, []() -> awaitable<void> {
    co_await example(ctx);
  }, [](std::exception_ptr e) {
    if (e) {
      try {
        std::rethrow_exception(e);
      } catch (const std::exception& ex) {
        std::cerr << "Error: " << ex.what() << std::endl;
      }
    }
  });
  
  ctx.run();
}
```

## Features

### Core Components
- **`io_context`** - Single-threaded event loop with io_uring/epoll backend
- **`awaitable<T>`** - Coroutine type for async operations with exception propagation
- **`co_spawn`** - Launch coroutines on an executor with multiple completion token support:
  - `use_detached` - Fire-and-forget (swallows exceptions)
  - `use_awaitable` - Returns an `awaitable<T>` you can `co_await` on
  - Custom completion handlers - `void(std::exception_ptr)` for error handling
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

### Concurrency Utilities
- **`when_all(...)`** - Wait for multiple awaitables to complete
  - Variadic template: `when_all(task1(), task2(), task3())`
  - Container support: `when_all(std::vector<awaitable<T>>)`
  - Returns tuple of results (variadic) or vector (container)
- **`when_any(...)`** - Wait for first awaitable to complete
  - Returns index and result of first completed task
  - Container support via `std::vector<awaitable<T>>`

### Other Utilities
- **`expected<T, E>`** - Result type for error handling
- **`work_guard<T>`** - RAII guard to keep event loop running
- **Timers** - Schedule callbacks with millisecond precision

## Design Principles

- **Single-threaded event loop model** - All operations on a given `io_context` execute on the same thread
- **Zero-allocation coroutines** - Efficient coroutine frame management with self-destroying tasks
- **Exception safety** - Proper exception propagation through coroutine boundaries
- **Header-only** - Easy integration, no separate compilation
- **Warning-free** - Compiles without `-Wsubobject-linkage` or ODR violations

## Advanced Examples

### Structured Concurrency with `use_awaitable`

```cpp
auto parent_task(io_context& ctx) -> awaitable<void> {
  // Spawn a child task and await its result
  auto result = co_await co_spawn(ctx, []() -> awaitable<int> {
    co_return 42;
  }, use_awaitable);
  
  std::cout << "Got result: " << result << std::endl;
}
```

### Concurrent Operations with `when_all`

```cpp
auto fetch_data(io_context& ctx) -> awaitable<void> {
  // Run multiple operations concurrently
  auto [result1, result2, result3] = co_await when_all(
    fetch_from_server1(ctx),
    fetch_from_server2(ctx),
    fetch_from_server3(ctx)
  );
  
  // All three operations complete before continuing
}

// Dynamic number of tasks
auto process_batch(io_context& ctx, std::vector<std::string> urls) -> awaitable<void> {
  std::vector<awaitable<Response>> tasks;
  for (const auto& url : urls) {
    tasks.push_back(fetch_url(ctx, url));
  }
  
  auto results = co_await when_all(std::move(tasks));
  // Process all results...
}
```

### Error Handling with Completion Handlers

```cpp
int main() {
  io_context ctx;
  
  co_spawn(ctx, []() -> awaitable<void> {
    // Your async code that might throw
    co_await risky_operation();
  }, [&](std::exception_ptr eptr) {
    if (eptr) {
      try {
        std::rethrow_exception(eptr);
      } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
      }
      ctx.stop(); // Stop event loop on error
    }
  });
  
  ctx.run();
}
```

## Missing Features

This is a minimal library focused on redisxz requirements. Notable omissions:

- **No UDP support**
- **No SSL/TLS** - Planned for redisxz
- **No acceptor/server sockets** - Client-only
- **No scatter-gather I/O**
- **No file I/O operations**
- **No async DNS resolution**
- **No cancellation tokens** - Can only close sockets
- **No strand/serialization executor**
- **No multi-threaded executor** - Single event loop per `io_context`
- **Limited IPv6 support** - Basic structures only

## Build Requirements

- **Platform**: Linux only
- **Compiler**: GCC 10+ (Clang/MSVC not tested)
- **C++ Standard**: C++20 (for coroutines)
- **CMake**: 3.15 or higher
- **Kernel**: Linux 5.1+ for io_uring support (5.4+ recommended), falls back to epoll on older kernels
- **Dependencies**:
  - [fmt](https://github.com/fmtlib/fmt) - Required only if `std::format` is not available (GCC 13+/Clang 15+ have it)
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

# Modern C++20 I/O Library for RedisXZ

This is a modern, Asio-inspired I/O library built specifically for the RedisXZ client using C++20 features.

## Design Philosophy

### Modern C++20 Features

1. **Coroutines** (`<coroutine>`)
   - All async operations return awaitable types
   - Use `co_await` for natural async code flow
   - `task<T>` type for composable async operations

2. **Concepts** (`<concepts>`)
   - Type-safe completion handlers
   - Compile-time validation of async operation requirements
   - Better error messages

3. **Structured Bindings**
   - Natural unpacking of results: `auto [ec, bytes] = co_await socket.async_read_some(...)`

4. **`[[nodiscard]]`**
   - All awaitable operations marked to prevent forgetting `co_await`

5. **`requires` Clauses**
   - Constrain template parameters for better interfaces

6. **`std::span`**
   - Zero-copy buffer views instead of raw pointers

## Core Components

### `io_context`
The execution context for all I/O operations. Similar to Asio's `io_context`.

```cpp
io_context ctx;
ctx.run();  // Run event loop
```

### `tcp_socket`
Asynchronous TCP socket with coroutine support:

```cpp
task<void> connect_example(io_context& ctx) {
    tcp_socket sock(ctx);

    // Connect with timeout
    co_await sock.async_connect(
        ip::tcp_endpoint{ip::address_v4::loopback(), 6379},
        std::chrono::seconds(5)
    );

    // Write data
    std::string cmd = "PING\r\n";
    co_await async_write(sock, std::span{cmd.data(), cmd.size()});

    // Read response
    char buf[1024];
    auto n = co_await sock.async_read_some(std::span{buf});
}
```

### `steady_timer`
High-precision timer:

```cpp
task<void> timer_example(io_context& ctx) {
    steady_timer timer(ctx);

    // Wait 100ms
    co_await timer.async_wait(std::chrono::milliseconds(100));
}
```

### `dynamic_buffer` & `static_buffer<N>`
Efficient buffer management:

```cpp
dynamic_buffer buf;
buf.append("Hello");
buf.append(" World");

auto data = buf.readable();  // std::span<char const>
auto view = buf.view();      // std::string_view

buf.consume(5);  // Remove "Hello"
```

### `awaitable_op<T>`
Base class for custom async operations:

```cpp
struct my_async_op : awaitable_op<int> {
    void start_operation() override {
        // Initiate operation
        // Later: complete(error_code{}, 42);
    }
};
```

## Usage Example

```cpp
#include <xz/io/io.hpp>
#include <iostream>

using namespace xz::io;

task<void> redis_ping(io_context& ctx) {
    tcp_socket sock(ctx);

    // Connect
    auto endpoint = ip::tcp_endpoint{
        ip::address_v4::loopback(),
        6379
    };

    co_await sock.async_connect(endpoint, std::chrono::seconds(5));

    // Send PING
    std::string cmd = "*1\r\n$4\r\nPING\r\n";
    co_await async_write(sock, std::span{cmd.data(), cmd.size()});

    // Read response
    dynamic_buffer response;
    char buf[1024];
    auto n = co_await sock.async_read_some(std::span{buf});
    response.append(std::span{buf, n});

    std::cout << "Response: " << response.view() << std::endl;
}

int main() {
    io_context ctx;

    auto t = redis_ping(ctx);
    t.resume();

    ctx.run();
    return 0;
}
```

## Advantages Over Traditional Callback-Based Approaches

1. **Readable Code**: Coroutines make async code look synchronous
2. **Error Handling**: Use try/catch naturally instead of checking every callback
3. **RAII**: Resources automatically cleaned up via destructors
4. **Type Safety**: Concepts catch errors at compile time
5. **Performance**: Zero-cost abstractions, no heap allocations for awaitables
6. **Composability**: Easy to build complex async flows

## Integration with Redis Client

The I/O library provides the foundation for:
- Connection management
- Request/response handling
- Pipelining
- Pub/sub
- Connection pooling

All using modern C++20 coroutines for clean, efficient code.

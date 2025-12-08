#include <xz/io/io.hpp>
#include <xz/io/src.hpp>

#include <iostream>
#include <string>

using namespace xz::io;

// Example 1: Simple timer
task<void> timer_example(io_context& ctx) {
  std::cout << "Timer example: waiting 100ms..." << std::endl;

  steady_timer timer(ctx);
  co_await timer.async_wait(std::chrono::milliseconds(100));

  std::cout << "Timer fired!" << std::endl;
}

// Example 2: TCP connection (will fail if no server, but demonstrates usage)
task<void> tcp_example(io_context& ctx) {
  std::cout << "TCP example: attempting connection..." << std::endl;

  try {
    tcp_socket sock(ctx);

    auto endpoint = ip::tcp_endpoint{ip::address_v4::loopback(), 6379};

    co_await sock.async_connect(endpoint);

    std::cout << "Connected to " << endpoint.to_string() << std::endl;

    // Send simple command
    std::string cmd = "PING\r\n";
    std::cout << "About to write " << cmd.size() << " bytes..." << std::endl;
    std::cout.flush();
    co_await async_write(sock, std::span{cmd.data(), cmd.size()});

    std::cout << "Sent PING command" << std::endl;
    std::cout.flush();

    // Read response
    char buf[1024];
    auto n = co_await sock.async_read_some(std::span{buf, sizeof(buf)});

    std::cout << "Received " << n << " bytes: "
              << std::string_view{buf, n} << std::endl;

    sock.close();

  } catch (std::system_error const& e) {
    std::cout << "Connection failed (expected if no Redis server): "
              << e.what() << std::endl;
  }

  ctx.stop();
}

// Example 3: Buffer usage
void buffer_example() {
  std::cout << "\nBuffer example:" << std::endl;

  dynamic_buffer buf;

  // Append data
  buf.append(std::string_view{"Hello, "});
  buf.append(std::string_view{"World!"});

  std::cout << "Buffer contains: " << buf.view() << std::endl;
  std::cout << "Size: " << buf.size() << " bytes" << std::endl;

  // Consume some data
  buf.consume(7);
  std::cout << "After consuming 7 bytes: " << buf.view() << std::endl;

  // Static buffer
  static_buffer<64> small_buf;
  auto span = small_buf.prepare(5);
  std::memcpy(span.data(), "Test", 4);
  small_buf.commit(4);

  auto readable = small_buf.readable();
  std::cout << "Static buffer: " << std::string_view{readable.data(), readable.size()} << std::endl;
}

// Example 4: IP address parsing
void ip_example() {
  std::cout << "\nIP address example:" << std::endl;

  auto ipv4 = ip::address_v4::from_string("192.168.1.1");
  std::cout << "IPv4: " << ipv4.to_string() << std::endl;

  auto loopback = ip::address_v4::loopback();
  std::cout << "IPv4 loopback: " << loopback.to_string() << std::endl;

  auto ipv6_loop = ip::address_v6::loopback();
  std::cout << "IPv6 loopback: " << ipv6_loop.to_string() << std::endl;

  ip::tcp_endpoint ep{ipv4, 8080};
  std::cout << "Endpoint: " << ep.to_string() << std::endl;
}

int main() {
  std::cout << "=== Modern C++20 I/O Library Examples ===" << std::endl;

  // Non-async examples
  buffer_example();
  ip_example();

  // Async examples with io_context
  std::cout << "\n=== Async Examples ===" << std::endl;

  io_context ctx;

  // Run timer example
  auto timer_task = timer_example(ctx);
  timer_task.resume();

  // Run TCP example (will fail gracefully if no server)
  auto tcp_task = tcp_example(ctx);
  tcp_task.resume();

  // Run event loop
  std::cout << "\nRunning event loop..." << std::endl;
  auto count = ctx.run();
  std::cout << "Event loop processed " << count << " events" << std::endl;

  std::cout << "\n=== All examples completed ===" << std::endl;

  return 0;
}

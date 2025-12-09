#include <xz/io/io.hpp>
#include <xz/io/src.hpp>

#include <fmt/format.h>

#include <iostream>
#include <string>
#include <string_view>
#include <vector>

using namespace xz::io;

/// Simple RESP3 protocol helper
class RespBuilder {
 public:
  /// Build RESP3 array command
  static auto build_array(std::vector<std::string> const& items) -> std::string {
    std::string result = fmt::format("*{}\r\n", items.size());
    for (auto const& item : items) {
      result += fmt::format("${}\r\n{}\r\n", item.size(), item);
    }
    return result;
  }

  /// Parse simple string response (+OK\r\n)
  static auto parse_simple_string(std::string_view resp) -> std::string {
    if (resp.empty() || resp[0] != '+') {
      return "";
    }
    auto end = resp.find("\r\n");
    if (end == std::string_view::npos) {
      return "";
    }
    return std::string{resp.substr(1, end - 1)};
  }

  /// Parse bulk string response ($5\r\nhello\r\n)
  static auto parse_bulk_string(std::string_view resp) -> std::string {
    if (resp.empty() || resp[0] != '$') {
      return "";
    }

    auto end = resp.find("\r\n");
    if (end == std::string_view::npos) {
      return "";
    }

    int len = std::stoi(std::string{resp.substr(1, end - 1)});
    if (len == -1) {
      return "";  // Null bulk string
    }

    auto start = end + 2;
    return std::string{resp.substr(start, len)};
  }
};

/// Redis client using modern C++20 coroutines
class RedisClient {
 public:
  explicit RedisClient(io_context& ctx) : socket_(ctx) {}

  /// Connect to Redis server
  auto connect(std::string const& host, uint16_t port) -> task<void> {
    auto addr = ip::address_v4::from_string(host);
    auto endpoint = ip::tcp_endpoint{addr, port};

    std::cout << fmt::format("Connecting to {}...\n", endpoint.to_string());
    co_await socket_.async_connect(endpoint, std::chrono::seconds(5));
    std::cout << "Connected!\n";
  }

  /// Send command and receive response
  auto send_command(std::vector<std::string> const& cmd) -> task<std::string> {
    // Build RESP3 command
    auto request = RespBuilder::build_array(cmd);

    std::cout << fmt::format("-> {}", cmd[0]);
    for (size_t i = 1; i < cmd.size(); ++i) {
      std::cout << fmt::format(" {}", cmd[i]);
    }
    std::cout << '\n';

    // Send command
    co_await async_write(socket_, std::span{request.data(), request.size()});

    // Read response
    char buffer[4096];
    auto n = co_await socket_.async_read_some(std::span{buffer, sizeof(buffer)});

    std::string response{buffer, n};
    std::cout << fmt::format("<- {}", response);

    co_return response;
  }

  /// Upgrade to RESP3 protocol
  auto hello() -> task<void> {
    std::vector<std::string> cmd = {"HELLO", "3"};
    auto response = co_await send_command(cmd);

    // RESP3 HELLO returns a map, but we'll just check it succeeded
    if (response.find('%') == 0 || response.find('+') == 0) {
      std::cout << "Upgraded to RESP3 protocol\n";
    }
  }

  /// SET key value
  auto set(std::string const& key, std::string const& value) -> task<void> {
    std::vector<std::string> cmd = {"SET", key, value};
    auto response = co_await send_command(cmd);

    auto result = RespBuilder::parse_simple_string(response);
    if (result == "OK") {
      std::cout << fmt::format("SET {} = {} succeeded\n", key, value);
    }
  }

  /// GET key
  auto get(std::string const& key) -> task<std::string> {
    std::vector<std::string> cmd = {"GET", key};
    auto response = co_await send_command(cmd);

    auto value = RespBuilder::parse_bulk_string(response);
    std::cout << fmt::format("GET {} = {}\n", key, value);

    co_return value;
  }

  /// PING
  auto ping() -> task<void> {
    std::vector<std::string> cmd = {"PING"};
    auto response = co_await send_command(cmd);
    std::cout << "PING received: " << response;
  }

  /// Close connection
  void disconnect() {
    if (socket_.is_open()) {
      std::cout << "Disconnecting...\n";
      socket_.close();
      std::cout << "Disconnected.\n";
    }
  }

 private:
  tcp_socket socket_;
};

/// Main client workflow
auto run_redis_client(io_context& ctx) -> task<void> {
  try {
    RedisClient client(ctx);

    // 1. Connect
    co_await client.connect("127.0.0.1", 6379);

    // 2. Upgrade to RESP3
    co_await client.hello();

    // 3. PING test
    co_await client.ping();

    // 4. SET some values
    co_await client.set("mykey", "Hello Redis!");
    co_await client.set("counter", "42");
    co_await client.set("name", "C++20 Client");

    // 5. GET values back
    auto value1 = co_await client.get("mykey");
    auto value2 = co_await client.get("counter");
    auto value3 = co_await client.get("name");

    // 6. Verify
    std::cout << "\n=== Verification ===\n";
    std::cout << fmt::format("mykey: {}\n", value1);
    std::cout << fmt::format("counter: {}\n", value2);
    std::cout << fmt::format("name: {}\n", value3);

    // 7. Disconnect
    client.disconnect();

    std::cout << "\n✓ All operations completed successfully!\n";

  } catch (std::system_error const& e) {
    std::cerr << fmt::format("Error: {} ({})\n", e.what(), e.code().message());
  } catch (std::exception const& e) {
    std::cerr << fmt::format("Error: {}\n", e.what());
  }

  ctx.stop();
}

int main() {
  std::cout << "=== Modern C++20 Redis Client ===\n\n";

  io_context ctx;

  // Start the client workflow
  auto client_task = run_redis_client(ctx);
  client_task.resume();

  // Run the event loop
  ctx.run();

  return 0;
}

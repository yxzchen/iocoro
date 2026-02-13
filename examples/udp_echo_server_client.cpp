#include <iocoro/iocoro.hpp>

#include <array>
#include <cstddef>
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using iocoro::ip::udp;

auto to_bytes(std::string_view text) -> std::vector<std::byte> {
  std::vector<std::byte> out(text.size());
  if (!text.empty()) {
    std::memcpy(out.data(), text.data(), text.size());
  }
  return out;
}

auto to_string(std::span<std::byte const> bytes) -> std::string {
  return {reinterpret_cast<char const*>(bytes.data()), bytes.size()};
}

auto udp_echo_server(udp::socket& server_socket) -> iocoro::awaitable<void> {
  for (;;) {
    std::array<std::byte, 512> recv_buf{};
    udp::endpoint source{iocoro::ip::address_v4::loopback(), 0};

    auto rr = co_await server_socket.async_receive_from(std::span{recv_buf}, source);
    if (!rr) {
      std::cerr << "udp_echo_server_client: server receive failed: " << rr.error().message()
                << "\n";
      co_return;
    }

    auto message = to_string(std::span<std::byte const>{recv_buf.data(), *rr});
    auto wr = co_await server_socket.async_send_to(std::span<std::byte const>{recv_buf.data(), *rr},
                                                   source);
    if (!wr) {
      std::cerr << "udp_echo_server_client: server send failed: " << wr.error().message() << "\n";
      co_return;
    }

    std::cout << "server <- " << message << "\n";
    if (message == "quit") {
      co_return;
    }
  }
}

auto udp_echo_client(udp::socket& client_socket,
                     udp::endpoint const& server_endpoint) -> iocoro::awaitable<void> {
  std::array<std::string_view, 3> messages{"ping", "iocoro", "quit"};
  for (auto const message : messages) {
    auto payload = to_bytes(message);
    auto wr =
      co_await client_socket.async_send_to(std::span<std::byte const>{payload}, server_endpoint);
    if (!wr) {
      std::cerr << "udp_echo_server_client: client send failed: " << wr.error().message() << "\n";
      co_return;
    }

    std::array<std::byte, 512> recv_buf{};
    udp::endpoint source{iocoro::ip::address_v4::loopback(), 0};
    auto rr = co_await client_socket.async_receive_from(std::span{recv_buf}, source);
    if (!rr) {
      std::cerr << "udp_echo_server_client: client receive failed: " << rr.error().message()
                << "\n";
      co_return;
    }

    std::cout << "client <- " << to_string(std::span<std::byte const>{recv_buf.data(), *rr})
              << "\n";
  }
}

auto co_main(iocoro::io_context& ctx) -> iocoro::awaitable<void> {
  udp::socket server_socket{ctx};
  udp::socket client_socket{ctx};

  auto bind_server = server_socket.bind(udp::endpoint{iocoro::ip::address_v4::loopback(), 0});
  if (!bind_server) {
    std::cerr << "udp_echo_server_client: bind server failed: " << bind_server.error().message()
              << "\n";
    ctx.stop();
    co_return;
  }

  auto bind_client = client_socket.bind(udp::endpoint{iocoro::ip::address_v4::loopback(), 0});
  if (!bind_client) {
    std::cerr << "udp_echo_server_client: bind client failed: " << bind_client.error().message()
              << "\n";
    ctx.stop();
    co_return;
  }

  auto server_ep = server_socket.local_endpoint();
  if (!server_ep) {
    std::cerr << "udp_echo_server_client: local_endpoint failed: " << server_ep.error().message()
              << "\n";
    ctx.stop();
    co_return;
  }

  auto server_join =
    iocoro::co_spawn(ctx.get_executor(), udp_echo_server(server_socket), iocoro::use_awaitable);
  co_await udp_echo_client(client_socket, *server_ep);
  co_await std::move(server_join);

  auto close_server = server_socket.close();
  if (!close_server) {
    std::cerr << "udp_echo_server_client: close server socket failed: "
              << close_server.error().message() << "\n";
  }

  auto close_client = client_socket.close();
  if (!close_client) {
    std::cerr << "udp_echo_server_client: close client socket failed: "
              << close_client.error().message() << "\n";
  }

  ctx.stop();
}

}  // namespace

int main() {
  iocoro::io_context ctx;
  iocoro::co_spawn(ctx.get_executor(), co_main(ctx), iocoro::detached);
  ctx.run();
  return 0;
}

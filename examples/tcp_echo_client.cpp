#include <iocoro/iocoro.hpp>

#include <cstdint>
#include <iostream>
#include <string>

namespace {

using iocoro::ip::tcp;

auto client_once(iocoro::io_context& ctx, tcp::endpoint ep) -> iocoro::awaitable<void> {
  tcp::socket socket{ctx};
  auto cr = co_await socket.async_connect(ep);
  if (!cr) {
    std::cerr << "tcp_echo_client: connect failed: " << cr.error().message() << "\n";
    co_return;
  }

  std::string msg = "ping\n";
  auto w = co_await iocoro::io::async_write(socket, iocoro::net::buffer(msg));
  if (!w) {
    std::cerr << "tcp_echo_client: write failed: " << w.error().message() << "\n";
    co_return;
  }

  std::string buffer(4096, '\0');
  auto buf = iocoro::net::buffer(buffer);
  auto r = co_await iocoro::io::async_read_until(socket, buf, '\n', 0);
  if (!r) {
    std::cerr << "tcp_echo_client: read_until failed: " << r.error().message() << "\n";
    co_return;
  }

  auto const n = *r;
  std::cout << "tcp_echo_client: received: " << buffer.substr(0, n);
}

}  // namespace

int main(int argc, char* argv[]) {
  std::uint16_t port = 55555;
  if (argc == 2) {
    port = static_cast<std::uint16_t>(std::stoi(argv[1]));
  }

  iocoro::io_context ctx;
  auto ep = tcp::endpoint{iocoro::ip::address_v4::loopback(), port};

  iocoro::co_spawn(ctx.get_executor(), client_once(ctx, ep), iocoro::detached);
  ctx.run();
  return 0;
}


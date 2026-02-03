#include <iocoro/iocoro.hpp>

#include <cstdint>
#include <iostream>
#include <string>

namespace {

using iocoro::ip::tcp;

auto server_once(iocoro::io_context& ctx, tcp::acceptor& acceptor) -> iocoro::awaitable<void> {
  auto accepted = co_await acceptor.async_accept();
  if (!accepted) {
    std::cerr << "tcp_echo_server: accept failed: " << accepted.error().message() << "\n";
    co_return;
  }

  auto cr = acceptor.close();
  if (!cr) {
    std::cerr << "tcp_echo_server: close acceptor failed: " << cr.error().message() << "\n";
  }

  auto socket = std::move(*accepted);
  std::string buffer(4096, '\0');
  auto buf = iocoro::net::buffer(buffer);

  auto r = co_await iocoro::io::async_read_until(socket, buf, '\n', 0);
  if (!r) {
    std::cerr << "tcp_echo_server: read_until failed: " << r.error().message() << "\n";
    co_return;
  }

  auto const n = *r;
  auto w = co_await iocoro::io::async_write(socket, iocoro::net::buffer(buffer.substr(0, n)));
  if (!w) {
    std::cerr << "tcp_echo_server: write failed: " << w.error().message() << "\n";
    co_return;
  }

  for (;;) {
    auto rr = co_await socket.async_read_some(buf);
    if (!rr) {
      std::cerr << "tcp_echo_server: read_some failed: " << rr.error().message() << "\n";
      break;
    }
    if (*rr == 0) {
      break;
    }
  }
}

}  // namespace

int main(int argc, char* argv[]) {
  std::uint16_t port = 55555;
  if (argc == 2) {
    port = static_cast<std::uint16_t>(std::stoi(argv[1]));
  }

  iocoro::io_context ctx;
  tcp::acceptor acceptor{ctx};

  auto ep = tcp::endpoint{iocoro::ip::address_v4::loopback(), port};
  auto lr = acceptor.listen(ep);
  if (!lr) {
    std::cerr << "tcp_echo_server: listen failed: " << lr.error().message() << "\n";
    return 1;
  }

  std::cout << "tcp_echo_server: listening on " << ep.to_string() << "\n";

  auto ex = ctx.get_executor();
  iocoro::co_spawn(ex, server_once(ctx, acceptor), iocoro::detached);

  ctx.run();
  return 0;
}

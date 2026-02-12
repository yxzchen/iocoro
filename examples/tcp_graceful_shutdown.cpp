#include <iocoro/iocoro.hpp>

#include <array>
#include <cstdint>
#include <iostream>
#include <string>

namespace {

using iocoro::ip::tcp;

auto graceful_server(tcp::acceptor& acceptor) -> iocoro::awaitable<void> {
  auto accepted = co_await acceptor.async_accept();
  if (!accepted) {
    std::cerr << "tcp_graceful_shutdown: accept failed: " << accepted.error().message() << "\n";
    co_return;
  }

  auto socket = std::move(*accepted);
  std::array<std::byte, 256> read_buf{};
  std::size_t total = 0;

  for (;;) {
    auto rr = co_await socket.async_read_some(std::span{read_buf});
    if (!rr) {
      std::cerr << "tcp_graceful_shutdown: server read failed: " << rr.error().message() << "\n";
      co_return;
    }
    if (*rr == 0) {
      break;
    }
    total += *rr;
  }

  std::string response = "server received " + std::to_string(total) + " bytes\n";
  auto wr = co_await iocoro::io::async_write(socket, iocoro::net::buffer(response));
  if (!wr) {
    std::cerr << "tcp_graceful_shutdown: server write failed: " << wr.error().message() << "\n";
    co_return;
  }

  auto sr = socket.shutdown(iocoro::shutdown_type::send);
  if (!sr) {
    std::cerr << "tcp_graceful_shutdown: server shutdown(send) failed: " << sr.error().message()
              << "\n";
  }
}

auto graceful_client(tcp::endpoint endpoint) -> iocoro::awaitable<void> {
  auto io_ex = co_await iocoro::this_coro::io_executor;
  tcp::socket socket{io_ex};

  auto cr = co_await socket.async_connect(endpoint);
  if (!cr) {
    std::cerr << "tcp_graceful_shutdown: client connect failed: " << cr.error().message() << "\n";
    co_return;
  }

  std::string payload = "graceful shutdown demo payload";
  auto wr = co_await iocoro::io::async_write(socket, iocoro::net::buffer(payload));
  if (!wr) {
    std::cerr << "tcp_graceful_shutdown: client write failed: " << wr.error().message() << "\n";
    co_return;
  }

  auto sr = socket.shutdown(iocoro::shutdown_type::send);
  if (!sr) {
    std::cerr << "tcp_graceful_shutdown: client shutdown(send) failed: " << sr.error().message()
              << "\n";
    co_return;
  }

  std::string line(256, '\0');
  auto rr = co_await iocoro::io::async_read_until(socket, iocoro::net::buffer(line), '\n');
  if (!rr) {
    std::cerr << "tcp_graceful_shutdown: client read failed: " << rr.error().message() << "\n";
    co_return;
  }

  std::cout << "tcp_graceful_shutdown: " << line.substr(0, *rr);
}

auto co_main(iocoro::io_context& ctx) -> iocoro::awaitable<void> {
  tcp::acceptor acceptor{ctx};
  auto lr = acceptor.listen(tcp::endpoint{iocoro::ip::address_v4::loopback(), 0});
  if (!lr) {
    std::cerr << "tcp_graceful_shutdown: listen failed: " << lr.error().message() << "\n";
    ctx.stop();
    co_return;
  }

  auto ep = acceptor.local_endpoint();
  if (!ep) {
    std::cerr << "tcp_graceful_shutdown: local_endpoint failed: " << ep.error().message() << "\n";
    ctx.stop();
    co_return;
  }

  auto server_join =
    iocoro::co_spawn(ctx.get_executor(), graceful_server(acceptor), iocoro::use_awaitable);
  co_await graceful_client(*ep);
  co_await std::move(server_join);

  auto close_r = acceptor.close();
  if (!close_r) {
    std::cerr << "tcp_graceful_shutdown: close acceptor failed: " << close_r.error().message()
              << "\n";
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

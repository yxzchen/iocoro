#include <iocoro/iocoro.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>

using namespace std::chrono_literals;

namespace {

using iocoro::ip::tcp;

auto sleepy_server(tcp::acceptor& acceptor) -> iocoro::awaitable<void> {
  auto accepted = co_await acceptor.async_accept();
  if (!accepted) {
    std::cerr << "tcp_timeout_cancel_client: accept failed: " << accepted.error().message() << "\n";
    co_return;
  }

  auto socket = std::move(*accepted);
  std::array<std::byte, 32> buf{};

  auto first = co_await socket.async_read_some(std::span{buf});
  if (!first) {
    std::cerr << "tcp_timeout_cancel_client: first read failed: " << first.error().message()
              << "\n";
    co_return;
  }

  auto second = co_await socket.async_read_some(std::span{buf});
  if (!second && second.error() != iocoro::error::operation_aborted) {
    std::cerr << "tcp_timeout_cancel_client: second read failed: " << second.error().message()
              << "\n";
  }
}

auto co_main(iocoro::io_context& ctx) -> iocoro::awaitable<void> {
  tcp::acceptor acceptor{ctx};
  auto listen_ep = tcp::endpoint{iocoro::ip::address_v4::loopback(), 0};
  auto lr = acceptor.listen(listen_ep);
  if (!lr) {
    std::cerr << "tcp_timeout_cancel_client: listen failed: " << lr.error().message() << "\n";
    ctx.stop();
    co_return;
  }

  auto local = acceptor.local_endpoint();
  if (!local) {
    std::cerr << "tcp_timeout_cancel_client: local_endpoint failed: " << local.error().message()
              << "\n";
    ctx.stop();
    co_return;
  }

  auto server_join =
    iocoro::co_spawn(ctx.get_executor(), sleepy_server(acceptor), iocoro::use_awaitable);

  tcp::socket client{ctx};
  auto cr = co_await client.async_connect(*local);
  if (!cr) {
    std::cerr << "tcp_timeout_cancel_client: connect failed: " << cr.error().message() << "\n";
    ctx.stop();
    co_return;
  }

  std::string ping = "ping\n";
  auto wr = co_await iocoro::io::async_write(client, iocoro::net::buffer(ping));
  if (!wr) {
    std::cerr << "tcp_timeout_cancel_client: write failed: " << wr.error().message() << "\n";
    ctx.stop();
    co_return;
  }

  std::string line(128, '\0');
  auto rr = co_await iocoro::with_timeout(
    iocoro::io::async_read_until(client, iocoro::net::buffer(line), '\n'), 200ms);

  if (!rr && rr.error() == iocoro::error::timed_out) {
    std::cout << "tcp_timeout_cancel_client: read timed out as expected\n";
  } else if (!rr) {
    std::cout << "tcp_timeout_cancel_client: read failed: " << rr.error().message() << "\n";
  } else {
    std::cout << "tcp_timeout_cancel_client: unexpected response: " << line.substr(0, *rr);
  }

  auto close_client = client.close();
  if (!close_client) {
    std::cerr << "tcp_timeout_cancel_client: close client failed: "
              << close_client.error().message() << "\n";
  }

  co_await std::move(server_join);
  auto close_acceptor = acceptor.close();
  if (!close_acceptor) {
    std::cerr << "tcp_timeout_cancel_client: close acceptor failed: "
              << close_acceptor.error().message() << "\n";
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

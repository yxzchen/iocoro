#include <iocoro/awaitable.hpp>
#include <iocoro/co_spawn.hpp>
#include <iocoro/completion_token.hpp>
#include <iocoro/io/read_until.hpp>
#include <iocoro/io/write.hpp>
#include <iocoro/io_context.hpp>
#include <iocoro/ip.hpp>
#include <iocoro/net/buffer.hpp>

#include <algorithm>
#include <cstddef>
#include <exception>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using iocoro::ip::tcp;

auto example(iocoro::io_context& ctx, tcp::endpoint ep, std::string msg, int n)
  -> iocoro::awaitable<void> {
  try {
    iocoro::ip::tcp::socket socket{ctx};
    if (!co_await socket.async_connect(ep)) {
      throw std::runtime_error("connect failed");
    }

    std::vector<std::byte> buffer(1024);
    for (int i = 0; i < n; ++i) {
      auto w = co_await iocoro::io::async_write(socket, iocoro::net::buffer(msg));
      if (!w) {
        throw std::runtime_error("write failed");
      }
      // std::fill(buffer.begin(), buffer.end(), std::byte{0});
      auto r = co_await iocoro::io::async_read_until(socket, std::span<std::byte>(buffer), '\n', 0);
      if (!r) {
        throw std::runtime_error("read failed");
      }
    }
  } catch (std::exception const& e) {
    std::cerr << "Error: " << e.what() << std::endl;
  }
}

}  // namespace

int main(int argc, char* argv[]) {
  try {
    int sessions = 1;
    int msgs = 1;

    if (argc == 3) {
      sessions = std::stoi(argv[1]);
      msgs = std::stoi(argv[2]);
    }

    iocoro::io_context ctx;
    auto ep = tcp::endpoint{iocoro::ip::address_v4::loopback(), 55555};

    auto ex = ctx.get_executor();
    for (int i = 0; i < sessions; ++i) {
      iocoro::co_spawn(ex, example(ctx, ep, "Some message\n", msgs), iocoro::detached);
    }

    ctx.run();
  } catch (std::exception const& e) {
    std::cerr << e.what() << std::endl;
  }
}

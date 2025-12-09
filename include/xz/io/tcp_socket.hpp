#pragma once

#include <xz/io/awaitable.hpp>
#include <xz/io/expected.hpp>
#include <xz/io/io_context.hpp>
#include <xz/io/ip.hpp>

#include <chrono>
#include <memory>
#include <span>
#include <system_error>

namespace xz::io {

namespace detail {
class tcp_socket_impl;
}

// Forward declaration
class tcp_socket;

/// Base class for async I/O operations with timeout support
template <typename Result>
class async_io_operation : public awaitable_op<Result> {
 protected:
  tcp_socket& socket_;
  std::chrono::milliseconds timeout_;
  detail::timer_handle timer_handle_;

  void setup_timeout();
  void cleanup_timer();

 public:
  async_io_operation(tcp_socket& s, std::chrono::milliseconds timeout, std::stop_token stop)
      : awaitable_op<Result>(std::move(stop)), socket_(s), timeout_(timeout) {}
};

/// Asynchronous TCP socket
class tcp_socket {
 public:
  explicit tcp_socket(io_context& ctx);
  ~tcp_socket();

  // Non-copyable, movable
  tcp_socket(tcp_socket const&) = delete;
  auto operator=(tcp_socket const&) -> tcp_socket& = delete;
  tcp_socket(tcp_socket&&) noexcept;
  auto operator=(tcp_socket&&) noexcept -> tcp_socket&;

  /// Get the associated io_context
  auto get_executor() noexcept -> io_context&;

  /// Check if socket is open
  auto is_open() const noexcept -> bool;

  /// Get native handle
  auto native_handle() const noexcept -> int;

  /// Close the socket
  void close();
  auto close_nothrow() noexcept -> expected<void, std::error_code>;

  /// Async operations

  /// Connect to remote endpoint (coroutine-based)
  struct [[nodiscard]] async_connect_op : async_io_operation<void> {
    async_connect_op(tcp_socket& s, ip::tcp_endpoint ep,
                     std::chrono::milliseconds timeout = {},
                     std::stop_token stop = {});

   protected:
    void start_operation() override;

   private:
    ip::tcp_endpoint endpoint_;
  };

  auto async_connect(ip::tcp_endpoint ep,
                    std::chrono::milliseconds timeout = {},
                    std::stop_token stop = {}) -> async_connect_op {
    return async_connect_op{*this, ep, timeout, std::move(stop)};
  }

  /// Read some data (coroutine-based)
  struct [[nodiscard]] async_read_some_op : async_io_operation<std::size_t> {
    async_read_some_op(tcp_socket& s, std::span<char> buf,
                       std::chrono::milliseconds timeout = {},
                       std::stop_token stop = {});

   protected:
    void start_operation() override;

   private:
    std::span<char> buffer_;
  };

  auto async_read_some(std::span<char> buffer,
                       std::chrono::milliseconds timeout = {},
                       std::stop_token stop = {}) -> async_read_some_op {
    return async_read_some_op{*this, buffer, timeout, std::move(stop)};
  }

  /// Write some data (coroutine-based)
  struct [[nodiscard]] async_write_some_op : async_io_operation<std::size_t> {
    async_write_some_op(tcp_socket& s, std::span<char const> buf,
                        std::chrono::milliseconds timeout = {},
                        std::stop_token stop = {});

   protected:
    void start_operation() override;

   private:
    std::span<char const> buffer_;
  };

  auto async_write_some(std::span<char const> buffer,
                        std::chrono::milliseconds timeout = {},
                        std::stop_token stop = {}) -> async_write_some_op {
    return async_write_some_op{*this, buffer, timeout, std::move(stop)};
  }

  /// Socket options
  auto set_option_nodelay(bool enable) -> expected<void, std::error_code>;
  auto set_option_keepalive(bool enable) -> expected<void, std::error_code>;
  auto set_option_reuseaddr(bool enable) -> expected<void, std::error_code>;

  /// Local/remote endpoints
  auto local_endpoint() const -> expected<ip::tcp_endpoint, std::error_code>;
  auto remote_endpoint() const -> expected<ip::tcp_endpoint, std::error_code>;

 private:
  friend struct async_connect_op;
  friend struct async_read_some_op;
  friend struct async_write_some_op;

  std::unique_ptr<detail::tcp_socket_impl> impl_;
};

/// Free functions for full read/write operations

/// Read exactly n bytes
inline auto async_read(tcp_socket& s, std::span<char> buffer,
                       std::chrono::milliseconds timeout = {},
                       std::stop_token stop = {}) -> task<void> {
  std::size_t total = 0;
  while (total < buffer.size()) {
    auto n = co_await s.async_read_some(buffer.subspan(total), timeout, stop);
    if (n == 0) throw std::system_error(make_error_code(error::eof));
    total += n;
  }
}

/// Write all data
inline auto async_write(tcp_socket& s, std::span<char const> buffer,
                        std::chrono::milliseconds timeout = {},
                        std::stop_token stop = {}) -> task<void> {
  std::size_t total = 0;
  while (total < buffer.size()) {
    auto n = co_await s.async_write_some(buffer.subspan(total), timeout, stop);
    total += n;
  }
}

/// Inline implementation of async_io_operation methods
template <typename Result>
void async_io_operation<Result>::setup_timeout() {
  if (timeout_.count() > 0) {
    timer_handle_ = socket_.get_executor().schedule_timer(
        timeout_,
        [this]() {
          socket_.get_executor().deregister_fd(socket_.native_handle());
          timer_handle_.reset();
          if constexpr (std::is_void_v<Result>) {
            this->complete(make_error_code(error::timeout));
          } else {
            this->complete(make_error_code(error::timeout), Result{});
          }
        });
  }
}

template <typename Result>
void async_io_operation<Result>::cleanup_timer() {
  if (timer_handle_) {
    socket_.get_executor().cancel_timer(timer_handle_);
    timer_handle_.reset();
  }
}

}  // namespace xz::io

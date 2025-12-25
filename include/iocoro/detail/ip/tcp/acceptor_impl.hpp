#pragma once

#include <iocoro/awaitable.hpp>
#include <iocoro/error.hpp>
#include <iocoro/executor.hpp>
#include <iocoro/expected.hpp>
#include <iocoro/ip/tcp/endpoint.hpp>

#include <iocoro/detail/socket/socket_impl_base.hpp>

#include <cstdint>
#include <mutex>
#include <system_error>

// Native socket APIs.
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>

namespace iocoro::detail::ip::tcp {

/// TCP acceptor implementation (IP-specific adapter).
class acceptor_impl {
 public:
  acceptor_impl() noexcept = default;
  explicit acceptor_impl(executor ex) noexcept : base_(ex) {}

  acceptor_impl(acceptor_impl const&) = delete;
  auto operator=(acceptor_impl const&) -> acceptor_impl& = delete;
  acceptor_impl(acceptor_impl&&) = delete;
  auto operator=(acceptor_impl&&) -> acceptor_impl& = delete;

  ~acceptor_impl() = default;

  auto get_executor() const noexcept -> executor { return base_.get_executor(); }
  auto native_handle() const noexcept -> int { return base_.native_handle(); }
  auto is_open() const noexcept -> bool { return base_.is_open(); }

  void cancel() noexcept {
    {
      std::scoped_lock lk{mtx_};
      ++accept_epoch_;
    }
    base_.cancel();
  }

  void close() noexcept {
    {
      std::scoped_lock lk{mtx_};
      ++accept_epoch_;
      listening_ = false;
      accept_in_flight_ = false;
    }
    base_.close();
  }

  template <class Option>
  auto set_option(Option const& opt) -> std::error_code {
    return base_.set_option(opt);
  }

  template <class Option>
  auto get_option(Option& opt) -> std::error_code {
    return base_.get_option(opt);
  }

  auto open(int family) -> std::error_code { return base_.open(family, SOCK_STREAM, IPPROTO_TCP); }

  auto bind(iocoro::ip::tcp::endpoint const& ep) -> std::error_code {
    auto const fd = base_.native_handle();
    if (fd < 0) {
      return error::not_open;
    }
    if (::bind(fd, ep.data(), ep.size()) != 0) {
      return std::error_code(errno, std::generic_category());
    }
    return {};
  }

  auto listen(int backlog) -> std::error_code {
    auto const fd = base_.native_handle();
    if (fd < 0) return error::not_open;
    if (backlog <= 0) backlog = SOMAXCONN;
    if (::listen(fd, backlog) != 0) {
      return std::error_code(errno, std::generic_category());
    }
    {
      std::scoped_lock lk{mtx_};
      listening_ = true;
    }
    return {};
  }

  auto local_endpoint() const -> expected<iocoro::ip::tcp::endpoint, std::error_code> {
    auto const fd = base_.native_handle();
    if (fd < 0) return unexpected(error::not_open);

    sockaddr_storage ss{};
    socklen_t len = sizeof(ss);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&ss), &len) != 0) {
      return unexpected(std::error_code(errno, std::generic_category()));
    }
    return iocoro::ip::tcp::endpoint::from_native(reinterpret_cast<sockaddr*>(&ss), len);
  }

  /// Accept a new connection.
  ///
  /// Returns:
  /// - a native connected fd on success (to be adopted by a `tcp::socket` implementation)
  /// - error_code on failure
  auto async_accept() -> awaitable<expected<int, std::error_code>> {
    auto const listen_fd = base_.native_handle();
    if (listen_fd < 0) {
      co_return unexpected(error::not_open);
    }

    std::uint64_t my_epoch = 0;
    {
      std::scoped_lock lk{mtx_};
      if (!listening_) {
        co_return unexpected(error::invalid_argument);
      }
      if (accept_in_flight_) {
        co_return unexpected(error::busy);
      }
      accept_in_flight_ = true;
      my_epoch = accept_epoch_;
    }

    auto guard = finally([this] {
      std::scoped_lock lk{mtx_};
      accept_in_flight_ = false;
    });

    for (;;) {
      int fd = ::accept4(listen_fd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);

      if (fd >= 0) {
        {
          std::scoped_lock lk{mtx_};
          if (accept_epoch_ != my_epoch) {
            (void)::close(fd);
            co_return unexpected(error::operation_aborted);
          }
        }
        co_return fd;
      }

      if (errno == EINTR) {
        continue;
      }

      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        auto ec = co_await base_.wait_read_ready();
        if (ec) {
          co_return unexpected(ec);
        }
        {
          std::scoped_lock lk{mtx_};
          if (accept_epoch_ != my_epoch) {
            co_return unexpected(error::operation_aborted);
          }
        }
        continue;
      }

      co_return unexpected(std::error_code(errno, std::generic_category()));
    }
  }

 private:
  template <class F>
  class final_action {
   public:
    explicit final_action(F f) noexcept : f_(std::move(f)) {}
    ~final_action() { f_(); }

   private:
    F f_;
  };
  template <class F>
  static auto finally(F f) noexcept -> final_action<F> {
    return final_action<F>(std::move(f));
  }

  socket::socket_impl_base base_{};
  mutable std::mutex mtx_{};
  bool listening_{false};
  bool accept_in_flight_{false};
  std::uint64_t accept_epoch_{0};
};

}  // namespace iocoro::detail::ip::tcp

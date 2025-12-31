#pragma once

#include <iocoro/assert.hpp>
#include <iocoro/awaitable.hpp>
#include <iocoro/error.hpp>
#include <iocoro/executor.hpp>
#include <iocoro/io_executor.hpp>
#include <iocoro/expected.hpp>

#include <iocoro/detail/socket/socket_impl_base.hpp>

#include <coroutine>
#include <cstdint>
#include <deque>
#include <mutex>
#include <system_error>

// Native socket APIs (generic / non-domain-specific).
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>

namespace iocoro::detail::net {

/// Generic acceptor implementation for sockaddr-based protocols, parameterized by Protocol.
///
/// Boundary:
/// - Depends on `Protocol::type()` / `Protocol::protocol()` only for socket creation.
/// - Endpoint semantics are NOT interpreted here; the endpoint is treated as a native view:
///   `data()/size()/family()` plus conversion helpers.
template <class Protocol>
class basic_acceptor_impl {
 public:
  using endpoint_type = typename Protocol::endpoint;

  basic_acceptor_impl() noexcept = delete;
  explicit basic_acceptor_impl(io_executor ex) noexcept : base_(ex) {}

  basic_acceptor_impl(basic_acceptor_impl const&) = delete;
  auto operator=(basic_acceptor_impl const&) -> basic_acceptor_impl& = delete;
  basic_acceptor_impl(basic_acceptor_impl&&) = delete;
  auto operator=(basic_acceptor_impl&&) -> basic_acceptor_impl& = delete;

  ~basic_acceptor_impl() = default;

  auto get_io_context_impl() const noexcept -> io_context_impl* { return base_.get_io_context_impl(); }
  auto native_handle() const noexcept -> int { return base_.native_handle(); }
  auto is_open() const noexcept -> bool { return base_.is_open(); }

  void cancel() noexcept { cancel_read(); }

  void cancel_read() noexcept;

  void cancel_write() noexcept { IOCORO_UNREACHABLE(); }

  void close() noexcept;

  template <class Option>
  auto set_option(Option const& opt) -> std::error_code {
    return base_.set_option(opt);
  }

  template <class Option>
  auto get_option(Option& opt) -> std::error_code {
    return base_.get_option(opt);
  }

  auto open(int family) -> std::error_code;

  auto bind(endpoint_type const& ep) -> std::error_code;

  auto listen(int backlog) -> std::error_code;

  auto local_endpoint() const -> expected<endpoint_type, std::error_code>;

  /// Accept a new connection.
  ///
  /// Returns:
  /// - a native connected fd on success (to be adopted by a stream socket)
  /// - error_code on failure
  auto async_accept() -> awaitable<expected<int, std::error_code>>;

 private:
  static auto set_nonblocking(int fd) noexcept -> bool;

  static auto set_cloexec(int fd) noexcept -> bool;

  struct accept_turn_state {
    std::coroutine_handle<> h{};
    any_executor ex{};
  };

  struct accept_turn_awaiter {
    basic_acceptor_impl* self;
    std::shared_ptr<accept_turn_state> st;

    accept_turn_awaiter(basic_acceptor_impl* self_, std::shared_ptr<accept_turn_state> st_)
        : self(self_), st(st_) {}

    bool await_ready() noexcept { return self->try_acquire_turn(st); }

    bool await_suspend(std::coroutine_handle<> h) noexcept {
      st->h = h;
      st->ex = detail::get_current_executor();
      return true;
    }

    void await_resume() noexcept {}
  };

  auto try_acquire_turn(std::shared_ptr<accept_turn_state> const& st) noexcept -> bool;

  void cleanup_expired_queue_front() noexcept;

  void complete_turn(std::shared_ptr<accept_turn_state> const& st) noexcept;

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

  socket::socket_impl_base base_;

  mutable std::mutex mtx_{};
  bool listening_{false};
  bool accept_active_{false};
  std::uint64_t accept_epoch_{0};

  std::deque<std::weak_ptr<accept_turn_state>> accept_queue_{};
};

}  // namespace iocoro::detail::net

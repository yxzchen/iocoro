#include <iocoro/error.hpp>

namespace iocoro {

namespace detail {

class error_category_impl : public std::error_category {
 public:
  auto name() const noexcept -> char const* override { return "iocoro"; }

  auto message(int ev) const -> std::string override {
    switch (static_cast<error>(ev)) {
      // Cancellation / internal / implementation status
      case error::operation_aborted:
        return "operation aborted";
      case error::timed_out:
        return "timed out";
      case error::internal_error:
        return "internal error";

      // Invalid input / unsupported / limits
      case error::invalid_argument:
        return "invalid argument";
      case error::invalid_endpoint:
        return "invalid endpoint";
      case error::unsupported_address_family:
        return "unsupported address family";
      case error::message_size:
        return "message size";

      // Object / socket state
      case error::not_open:
        return "resource not open";
      case error::already_open:
        return "already open";
      case error::busy:
        return "resource busy";
      case error::not_bound:
        return "not bound";
      case error::not_listening:
        return "not listening";

      // Connection state
      case error::not_connected:
        return "not connected";
      case error::already_connected:
        return "already connected";

      // Stream / transport outcomes
      case error::eof:
        return "end of file";
      case error::broken_pipe:
        return "broken pipe";
      case error::connection_reset:
        return "connection reset";

      // Network-related
      case error::address_in_use:
        return "address in use";
      case error::address_not_available:
        return "address not available";
      case error::network_unreachable:
        return "network unreachable";
      case error::host_unreachable:
        return "host unreachable";
      case error::connection_refused:
        return "connection refused";
      case error::connection_aborted:
        return "connection aborted";
      case error::connection_timed_out:
        return "connection timed out";
      default:
        return "unknown error";
    }
  }
};

inline auto error_category() -> std::error_category const& {
  static error_category_impl instance;
  return instance;
}

}  // namespace detail

inline auto make_error_code(error e) -> std::error_code {
  return {static_cast<int>(e), detail::error_category()};
}

}  // namespace iocoro

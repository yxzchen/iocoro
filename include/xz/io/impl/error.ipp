#pragma once

#include <xz/io/error.hpp>

namespace xz::io {

namespace detail {

class error_category_impl : public std::error_category {
 public:
  auto name() const noexcept -> char const* override { return "xz::io"; }

  auto message(int ev) const -> std::string override {
    switch (static_cast<error>(ev)) {
      case error::operation_aborted:
        return "operation aborted";
      case error::connection_refused:
        return "connection refused";
      case error::connection_reset:
        return "connection reset";
      case error::timeout:
        return "timeout";
      case error::eof:
        return "end of file";
      case error::not_connected:
        return "not connected";
      case error::already_connected:
        return "already connected";
      case error::address_in_use:
        return "address in use";
      case error::network_unreachable:
        return "network unreachable";
      case error::host_unreachable:
        return "host unreachable";
      case error::invalid_argument:
        return "invalid argument";
      case error::resolve_failed:
        return "name resolution failed";
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

auto make_error_code(error e) -> std::error_code {
  return {static_cast<int>(e), detail::error_category()};
}

}  // namespace xz::io

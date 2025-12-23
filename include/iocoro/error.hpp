#pragma once

#include <system_error>
#include <type_traits>

namespace iocoro {

enum class error {
  /// Operation cancelled
  operation_aborted = 1,

  /// Feature exists in API but is not implemented yet (stubs / WIP)
  not_implemented,

  /// Invalid argument / malformed input (library-level)
  invalid_argument,

  /// The socket (or underlying resource) is not open.
  not_open,

  /// An operation cannot proceed because another conflicting operation is in-flight.
  busy,

  /// Socket is not connected.
  not_connected,

  /// Socket is already connected.
  already_connected,

  /// Endpoint is invalid or unsupported for the requested operation.
  invalid_endpoint,

  /// Address family is not supported by this object/backend.
  unsupported_address_family,
};

auto make_error_code(error e) -> std::error_code;

}  // namespace iocoro

namespace std {

template <>
struct is_error_code_enum<iocoro::error> : std::true_type {};

}  // namespace std

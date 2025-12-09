#pragma once

#include <system_error>
#include <type_traits>

namespace xz::io {

enum class error {
  /// Operation cancelled
  operation_aborted = 1,

  /// Operation cancelled
  operation_failed = 1,

  /// Connection refused
  connection_refused,

  /// Connection reset
  connection_reset,

  /// Connection timeout
  timeout,

  /// End of file
  eof,

  /// Not connected
  not_connected,

  /// Already connected
  already_connected,

  /// Address in use
  address_in_use,

  /// Network unreachable
  network_unreachable,

  /// Host unreachable
  host_unreachable,

  /// Invalid argument
  invalid_argument,

  /// Name resolution failed
  resolve_failed,
};

auto make_error_code(error e) -> std::error_code;

}  // namespace xz::io

namespace std {

template <>
struct is_error_code_enum<xz::io::error> : std::true_type {};

}  // namespace std

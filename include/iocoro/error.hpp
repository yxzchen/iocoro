#pragma once

#include <system_error>
#include <type_traits>

namespace iocoro {

/// Library error codes for common asynchronous I/O failures.
///
/// These values are converted to `std::error_code` via `make_error_code(error)` and a custom
/// error category (see `impl/error.ipp`).
enum class error {
  // Cancellation / timeouts / internal
  /// Operation cancelled.
  operation_aborted = 1,

  /// Operation timed out (library-level, e.g. `with_timeout`).
  timed_out,

  /// Internal error (unexpected exception or system failure).
  internal_error,

  // Invalid input / unsupported / limits
  /// Invalid argument / malformed input (library-level).
  invalid_argument,

  /// Endpoint is invalid or unsupported for the requested operation.
  invalid_endpoint,

  /// Address family is not supported by this object/backend.
  unsupported_address_family,

  /// Operation failed because a message/buffer would exceed the allowed maximum size.
  message_size,

  // Object / socket state
  /// The socket (or underlying resource) is not open.
  not_open,

  /// Operation failed because the resource is already open.
  already_open,

  /// An operation cannot proceed because another conflicting operation is in-flight.
  busy,

  /// Datagram socket has no local address (required for receiving).
  not_bound,

  /// Acceptor is open/bound but not in listening state (listen() not called successfully).
  not_listening,

  // Connection state / stream outcomes
  /// Socket is not connected.
  not_connected,

  /// Socket is already connected.
  already_connected,

  /// End of file / orderly shutdown by peer (read returned 0).
  eof,

  /// Write failed because the peer has closed the connection / write end is shut down.
  broken_pipe,

  /// Connection was reset by peer.
  connection_reset,

  // Network-related (normalized from common errno values)
  /// Address is already in use (e.g. bind()).
  address_in_use,

  /// Address is not available on the local machine (e.g. bind()).
  address_not_available,

  /// Network is unreachable.
  network_unreachable,

  /// Host is unreachable.
  host_unreachable,

  /// Connection attempt failed because the peer refused it.
  connection_refused,

  /// Connection was aborted (e.g. during accept/connect).
  connection_aborted,

  /// Connection attempt timed out at the OS level (e.g. connect()).
  connection_timed_out,

};

auto make_error_code(error e) -> std::error_code;

}  // namespace iocoro

namespace std {

template <>
struct is_error_code_enum<iocoro::error> : std::true_type {};

}  // namespace std

#include <iocoro/impl/error.ipp>

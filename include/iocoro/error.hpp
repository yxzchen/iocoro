#pragma once

#include <system_error>
#include <type_traits>

namespace iocoro {

enum class error {
  /// Operation cancelled.
  operation_aborted = 1,

  /// Feature exists in API but is not implemented yet (stubs / WIP).
  not_implemented,

  /// Internal error (unexpected exception or system failure).
  internal_error,

  /// Invalid argument / malformed input (library-level).
  invalid_argument,

  /// Endpoint is invalid or unsupported for the requested operation.
  invalid_endpoint,

  /// Address family is not supported by this object/backend.
  unsupported_address_family,

  /// Operation failed because a message/buffer would exceed the allowed maximum size.
  message_size,

  /// The socket (or underlying resource) is not open.
  not_open,

  /// An operation cannot proceed because another conflicting operation is in-flight.
  busy,

  /// Datagram socket is not bound to a local address (required for receiving).
  not_bound,

  /// Acceptor is open/bound but not in listening state (listen() not called successfully).
  not_listening,

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

  /// Operation timed out.
  timed_out,
};

auto make_error_code(error e) -> std::error_code;

}  // namespace iocoro

namespace std {

template <>
struct is_error_code_enum<iocoro::error> : std::true_type {};

}  // namespace std

#include <iocoro/impl/error.ipp>

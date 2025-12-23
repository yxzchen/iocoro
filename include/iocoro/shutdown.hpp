#pragma once

namespace iocoro {

/// Socket shutdown direction.
///
/// This is protocol-agnostic and maps to platform shutdown flags in implementations.
enum class shutdown_type {
  receive,
  send,
  both,
};

}  // namespace iocoro

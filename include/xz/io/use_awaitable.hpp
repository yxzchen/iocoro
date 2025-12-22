#pragma once

namespace xz::io {

/// Completion token that selects coroutine-based async operations.
struct use_awaitable_t {};

inline constexpr use_awaitable_t use_awaitable{};

}  // namespace xz::io

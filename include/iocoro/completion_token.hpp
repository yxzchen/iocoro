#pragma once

namespace iocoro {

/// Completion token that selects detached (fire-and-forget) execution.
struct detached_t {};
inline constexpr detached_t detached{};

/// Completion token that selects coroutine-based async operations.
struct use_awaitable_t {};
inline constexpr use_awaitable_t use_awaitable{};

}  // namespace iocoro

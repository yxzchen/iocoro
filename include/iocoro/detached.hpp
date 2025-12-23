#pragma once

namespace iocoro {

/// Completion token that selects detached (fire-and-forget) execution.
struct detached_t {};

inline constexpr detached_t detached{};

}  // namespace iocoro

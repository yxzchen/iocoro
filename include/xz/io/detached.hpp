#pragma once

namespace xz::io {

/// Completion token that selects detached (fire-and-forget) execution.
struct detached_t {};

inline constexpr detached_t detached{};

}  // namespace xz::io

#pragma once

#include <cstdlib>
#include <exception>

// Assertion/ensure utilities used for internal invariants and API contracts.
//
// - `IOCORO_ASSERT(...)`: debug-only assertion (compiled out in NDEBUG).
// - `IOCORO_ENSURE(...)`: always-on contract check; fails fast (terminates) on violation.
// - `IOCORO_UNREACHABLE()`: mark unreachable control flow; fails fast if reached.
//
// IMPORTANT: These macros are part of the library's safety story; prefer `IOCORO_ENSURE`
// for conditions that must hold in production builds.
#if defined(__GNUC__) || defined(__clang__)
#define IOCORO_LIKELY(x) __builtin_expect(!!(x), 1)
#define IOCORO_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define IOCORO_LIKELY(x) (x)
#define IOCORO_UNLIKELY(x) (x)
#endif

namespace iocoro::detail {

[[noreturn]] void assert_fail(char const* expr, char const* file, int line,
                              char const* func) noexcept;

[[noreturn]] void assert_fail(char const* expr, char const* msg, char const* file, int line,
                              char const* func) noexcept;

[[noreturn]] void ensure_fail(char const* expr, char const* file, int line,
                              char const* func) noexcept;

[[noreturn]] void ensure_fail(char const* expr, char const* msg, char const* file, int line,
                              char const* func) noexcept;

[[noreturn]] void unreachable_fail(char const* file, int line, char const* func) noexcept;

}  // namespace iocoro::detail

// -------------------- ASSERT --------------------
#if !defined(NDEBUG)

#define IOCORO_ASSERT_SELECTOR(_1, _2, NAME, ...) NAME

#define IOCORO_ASSERT_1(expr)    \
  (IOCORO_LIKELY(expr) ? (void)0 \
                       : ::iocoro::detail::assert_fail(#expr, __FILE__, __LINE__, __func__))

#define IOCORO_ASSERT_2(expr, msg) \
  (IOCORO_LIKELY(expr) ? (void)0   \
                       : ::iocoro::detail::assert_fail(#expr, msg, __FILE__, __LINE__, __func__))

#define IOCORO_ASSERT(...) \
  IOCORO_ASSERT_SELECTOR(__VA_ARGS__, IOCORO_ASSERT_2, IOCORO_ASSERT_1)(__VA_ARGS__)

#else
#define IOCORO_ASSERT(...) ((void)0)
#endif

// -------------------- ENSURE --------------------

#define IOCORO_ENSURE_SELECTOR(_1, _2, NAME, ...) NAME

#define IOCORO_ENSURE_1(expr)    \
  (IOCORO_LIKELY(expr) ? (void)0 \
                       : ::iocoro::detail::ensure_fail(#expr, __FILE__, __LINE__, __func__))

#define IOCORO_ENSURE_2(expr, msg) \
  (IOCORO_LIKELY(expr) ? (void)0   \
                       : ::iocoro::detail::ensure_fail(#expr, msg, __FILE__, __LINE__, __func__))

#define IOCORO_ENSURE(...) \
  IOCORO_ENSURE_SELECTOR(__VA_ARGS__, IOCORO_ENSURE_2, IOCORO_ENSURE_1)(__VA_ARGS__)

// -------------------- UNREACHABLE --------------------

#define IOCORO_UNREACHABLE() ::iocoro::detail::unreachable_fail(__FILE__, __LINE__, __func__)

#include <iocoro/impl/assert.ipp>

#pragma once

#include <cstdlib>
#include <exception>

#if defined(__GNUC__) || defined(__clang__)
#define IOXZ_LIKELY(x) __builtin_expect(!!(x), 1)
#define IOXZ_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define IOXZ_LIKELY(x) (x)
#define IOXZ_UNLIKELY(x) (x)
#endif

namespace xz::io {

namespace detail {

[[noreturn]] void assert_fail(char const* expr, char const* file, int line,
                              char const* func) noexcept;

[[noreturn]] void ensure_fail(char const* expr, char const* file, int line,
                              char const* func) noexcept;

[[noreturn]] void unreachable_fail(char const* file, int line, char const* func) noexcept;

}  // namespace detail

}  // namespace xz::io

// -------------------- ASSERT --------------------

#if !defined(NDEBUG)

#define IOXZ_ASSERT(expr) \
  (IOXZ_LIKELY(expr) ? (void)0 : ::xz::io::detail::assert_fail(#expr, __FILE__, __LINE__, __func__))

#else

#define IOXZ_ASSERT(expr) ((void)0)

#endif

// -------------------- ENSURE --------------------

#define IOXZ_ENSURE(expr) \
  (IOXZ_LIKELY(expr) ? (void)0 : ::xz::io::detail::ensure_fail(#expr, __FILE__, __LINE__, __func__))

// -------------------- UNREACHABLE --------------------

#define IOXZ_UNREACHABLE() ::xz::io::detail::unreachable_fail(__FILE__, __LINE__, __func__)

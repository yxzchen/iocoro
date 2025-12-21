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

[[noreturn]] void assert_fail(char const* expr, char const* msg, char const* file, int line,
                              char const* func) noexcept;

[[noreturn]] void ensure_fail(char const* expr, char const* file, int line,
                              char const* func) noexcept;

[[noreturn]] void ensure_fail(char const* expr, char const* msg, char const* file, int line,
                              char const* func) noexcept;

}  // namespace detail
}  // namespace xz::io

// -------------------- ASSERT --------------------
#if !defined(NDEBUG)

#define IOXZ_ASSERT_SELECTOR(_1, _2, NAME, ...) NAME

#define IOXZ_ASSERT_1(expr) \
  (IOXZ_LIKELY(expr) ? (void)0 : ::xz::io::detail::assert_fail(#expr, __FILE__, __LINE__, __func__))

#define IOXZ_ASSERT_2(expr, msg) \
  (IOXZ_LIKELY(expr) ? (void)0   \
                     : ::xz::io::detail::assert_fail(#expr, msg, __FILE__, __LINE__, __func__))

#define IOXZ_ASSERT(...) \
  IOXZ_ASSERT_SELECTOR(__VA_ARGS__, IOXZ_ASSERT_2, IOXZ_ASSERT_1)(__VA_ARGS__)

#else
#define IOXZ_ASSERT(...) ((void)0)
#endif

// -------------------- ENSURE --------------------

#define IOXZ_ENSURE_SELECTOR(_1, _2, NAME, ...) NAME

#define IOXZ_ENSURE_1(expr) \
  (IOXZ_LIKELY(expr) ? (void)0 : ::xz::io::detail::ensure_fail(#expr, __FILE__, __LINE__, __func__))

#define IOXZ_ENSURE_2(expr, msg) \
  (IOXZ_LIKELY(expr) ? (void)0   \
                     : ::xz::io::detail::ensure_fail(#expr, msg, __FILE__, __LINE__, __func__))

#define IOXZ_ENSURE(...) \
  IOXZ_ENSURE_SELECTOR(__VA_ARGS__, IOXZ_ENSURE_2, IOXZ_ENSURE_1)(__VA_ARGS__)

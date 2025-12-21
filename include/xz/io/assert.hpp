#pragma once

#include <cstdlib>
#include <exception>

#if defined(__GNUC__) || defined(__clang__)
#define XZ_LIKELY(x) __builtin_expect(!!(x), 1)
#define XZ_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define XZ_LIKELY(x) (x)
#define XZ_UNLIKELY(x) (x)
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

#define XZ_ASSERT_SELECTOR(_1, _2, NAME, ...) NAME

#define XZ_ASSERT_1(expr) \
  (XZ_LIKELY(expr) ? (void)0 : ::xz::io::detail::assert_fail(#expr, __FILE__, __LINE__, __func__))

#define XZ_ASSERT_2(expr, msg) \
  (XZ_LIKELY(expr) ? (void)0   \
                     : ::xz::io::detail::assert_fail(#expr, msg, __FILE__, __LINE__, __func__))

#define XZ_ASSERT(...) \
  XZ_ASSERT_SELECTOR(__VA_ARGS__, XZ_ASSERT_2, XZ_ASSERT_1)(__VA_ARGS__)

#else
#define XZ_ASSERT(...) ((void)0)
#endif

// -------------------- ENSURE --------------------

#define XZ_ENSURE_SELECTOR(_1, _2, NAME, ...) NAME

#define XZ_ENSURE_1(expr) \
  (XZ_LIKELY(expr) ? (void)0 : ::xz::io::detail::ensure_fail(#expr, __FILE__, __LINE__, __func__))

#define XZ_ENSURE_2(expr, msg) \
  (XZ_LIKELY(expr) ? (void)0   \
                     : ::xz::io::detail::ensure_fail(#expr, msg, __FILE__, __LINE__, __func__))

#define XZ_ENSURE(...) \
  XZ_ENSURE_SELECTOR(__VA_ARGS__, XZ_ENSURE_2, XZ_ENSURE_1)(__VA_ARGS__)

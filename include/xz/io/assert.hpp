#pragma once

#include <cstdlib>
#include <exception>

#if defined(__GNUC__) || defined(__clang__)
#define COROX_LIKELY(x) __builtin_expect(!!(x), 1)
#define COROX_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define COROX_LIKELY(x) (x)
#define COROX_UNLIKELY(x) (x)
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

#define COROX_ASSERT_SELECTOR(_1, _2, NAME, ...) NAME

#define COROX_ASSERT_1(expr) \
  (COROX_LIKELY(expr) ? (void)0 : ::xz::io::detail::assert_fail(#expr, __FILE__, __LINE__, __func__))

#define COROX_ASSERT_2(expr, msg) \
  (COROX_LIKELY(expr) ? (void)0   \
                     : ::xz::io::detail::assert_fail(#expr, msg, __FILE__, __LINE__, __func__))

#define COROX_ASSERT(...) \
  COROX_ASSERT_SELECTOR(__VA_ARGS__, COROX_ASSERT_2, COROX_ASSERT_1)(__VA_ARGS__)

#else
#define COROX_ASSERT(...) ((void)0)
#endif

// -------------------- ENSURE --------------------

#define COROX_ENSURE_SELECTOR(_1, _2, NAME, ...) NAME

#define COROX_ENSURE_1(expr) \
  (COROX_LIKELY(expr) ? (void)0 : ::xz::io::detail::ensure_fail(#expr, __FILE__, __LINE__, __func__))

#define COROX_ENSURE_2(expr, msg) \
  (COROX_LIKELY(expr) ? (void)0   \
                     : ::xz::io::detail::ensure_fail(#expr, msg, __FILE__, __LINE__, __func__))

#define COROX_ENSURE(...) \
  COROX_ENSURE_SELECTOR(__VA_ARGS__, COROX_ENSURE_2, COROX_ENSURE_1)(__VA_ARGS__)

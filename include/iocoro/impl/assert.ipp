#include <iocoro/assert.hpp>

#include <cstdio>
#include <cstdlib>

namespace iocoro::detail {

namespace {

[[noreturn]] void fail_impl(char const* kind, char const* expr, char const* msg, char const* file,
                            int line, char const* func) noexcept {
  if (msg) {
    std::fprintf(stderr,
                 "[iocoro] %s failure\n"
                 "  expression: %s\n"
                 "  message   : %s\n"
                 "  location  : %s:%d\n"
                 "  function  : %s\n",
                 kind, expr ? expr : "(none)", msg, file, line, func);
  } else {
    std::fprintf(stderr,
                 "[iocoro] %s failure\n"
                 "  expression: %s\n"
                 "  location  : %s:%d\n"
                 "  function  : %s\n",
                 kind, expr ? expr : "(none)", file, line, func);
  }
  std::fflush(stderr);
  std::abort();
}

}  // namespace

// -------------------- ASSERT --------------------

void assert_fail(char const* expr, char const* file, int line, char const* func) noexcept {
  fail_impl("ASSERT", expr, nullptr, file, line, func);
}

void assert_fail(char const* expr, char const* msg, char const* file, int line,
                 char const* func) noexcept {
  fail_impl("ASSERT", expr, msg, file, line, func);
}

// -------------------- ENSURE --------------------

void ensure_fail(char const* expr, char const* file, int line, char const* func) noexcept {
  fail_impl("ENSURE", expr, nullptr, file, line, func);
}

void ensure_fail(char const* expr, char const* msg, char const* file, int line,
                 char const* func) noexcept {
  fail_impl("ENSURE", expr, msg, file, line, func);
}

void unreachable_fail(char const* file, int line, char const* func) noexcept {
  fail_impl("UNREACHABLE", nullptr, nullptr, file, line, func);
}

}  // namespace iocoro::detail

#include <xz/io/assert.hpp>

#include <cstdio>
#include <cstdlib>

namespace xz::io::detail {

namespace {

[[noreturn]] void fail(char const* kind, char const* expr, char const* msg, char const* file,
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
  fail("ASSERT", expr, nullptr, file, line, func);
}

void assert_fail(char const* expr, char const* msg, char const* file, int line,
                 char const* func) noexcept {
  fail("ASSERT", expr, msg, file, line, func);
}

// -------------------- ENSURE --------------------

void ensure_fail(char const* expr, char const* file, int line, char const* func) noexcept {
  fail("ENSURE", expr, nullptr, file, line, func);
}

void ensure_fail(char const* expr, char const* msg, char const* file, int line,
                 char const* func) noexcept {
  fail("ENSURE", expr, msg, file, line, func);
}

void unreachable_fail(char const* file, int line, char const* func) noexcept {
  fail("UNREACHABLE", nullptr, nullptr, file, line, func);
}

}  // namespace xz::io::detail

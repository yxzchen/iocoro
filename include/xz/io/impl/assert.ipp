#include <xz/io/assert.hpp>

#include <cstdio>
#include <cstdlib>

namespace xz::io::detail {

namespace {

[[noreturn]] void fail(char const* kind, char const* expr, char const* file, int line,
                       char const* func) noexcept {
  std::fprintf(stderr,
               "[xz::io] %s failure\n"
               "  expression: %s\n"
               "  location  : %s:%d\n"
               "  function  : %s\n",
               kind, expr ? expr : "(none)", file, line, func);

  std::fflush(stderr);
  std::abort();
}

}  // namespace

void assert_fail(char const* expr, char const* file, int line, char const* func) noexcept {
  fail("ASSERT", expr, file, line, func);
}

void ensure_fail(char const* expr, char const* file, int line, char const* func) noexcept {
  fail("ENSURE", expr, file, line, func);
}

void unreachable_fail(char const* file, int line, char const* func) noexcept {
  fail("UNREACHABLE", nullptr, file, line, func);
}

}  // namespace xz::io::detail

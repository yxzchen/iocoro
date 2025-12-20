#include <xz/io/executor.hpp>
#include <xz/io/io_context.hpp>

namespace xz::io {

inline auto io_context::get_executor() noexcept -> executor {
  return executor{*this};
}

}  // namespace xz::io

#include <iocoro/thread_pool.hpp>
#include <iocoro/thread_pool_executor.hpp>

namespace iocoro {

inline auto thread_pool::get_executor() noexcept -> thread_pool_executor {
  return thread_pool_executor{*this};
}

}  // namespace iocoro

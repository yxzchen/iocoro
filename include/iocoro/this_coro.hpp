#pragma once

namespace iocoro::this_coro {

struct executor_t {};
inline constexpr executor_t executor{};

struct io_executor_t {};
inline constexpr io_executor_t io_executor{};

}  // namespace iocoro::this_coro

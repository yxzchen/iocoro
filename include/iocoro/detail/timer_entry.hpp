#pragma once

#include <cstdint>

namespace iocoro::detail {

enum class timer_state : std::uint8_t {
  pending,
  fired,
  cancelled,
};

}  // namespace iocoro::detail

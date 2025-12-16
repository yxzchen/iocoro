#pragma once

#include <xz/io/awaitable.hpp>

#include <type_traits>

namespace xz::io::detail {

template <typename>
struct is_awaitable : std::false_type {};

template <typename T>
struct is_awaitable<awaitable<T>> : std::true_type {};

template <typename T>
inline constexpr bool is_awaitable_v = is_awaitable<std::remove_cvref_t<T>>::value;

template <typename>
struct awaitable_result;

template <typename T>
struct awaitable_result<awaitable<T>> {
  using type = T;
};

template <typename T>
using awaitable_result_t = typename awaitable_result<std::remove_cvref_t<T>>::type;

}  // namespace xz::io::detail



#pragma once

#include <type_traits>

namespace iocoro {

// Forward declaration
template <typename T>
class awaitable;

namespace traits {

/// Type trait to extract the value type from an awaitable type.
///
/// Primary template is undefined; specializations must define a `type` member.
template <typename A>
struct awaitable_result;

/// Specialization for iocoro::awaitable<T>: extracts the inner type T.
template <typename T>
struct awaitable_result<awaitable<T>> {
  using type = T;
};

/// Helper alias that strips cv-qualifiers and references before trait lookup.
///
/// This ensures that `awaitable<T>&`, `const awaitable<T>`, etc., all resolve
/// to the same underlying value type T.
template <typename A>
using awaitable_result_t = typename awaitable_result<std::remove_cvref_t<A>>::type;

}  // namespace traits
}  // namespace iocoro

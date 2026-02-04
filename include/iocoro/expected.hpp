#pragma once

#include <exception>
#include <functional>
#include <type_traits>
#include <utility>
#include <variant>

#include <version>
#if defined(__cpp_lib_expected) && __cpp_lib_expected >= 202211L
#include <expected>
#endif

namespace iocoro {

// Compatibility shim for `std::expected` (C++23).
//
// - If the standard library provides `std::expected`, this header aliases it.
// - Otherwise, this header provides a small subset implementation sufficient for iocoro.
//
// This is part of the library's public error model; see `result<T>` in `result.hpp`.
#if defined(__cpp_lib_expected) && __cpp_lib_expected >= 202211L

template <class E>
using unexpected = std::unexpected<E>;

using unexpect_t = std::unexpect_t;
inline constexpr unexpect_t unexpect = std::unexpect;

template <class T, class E>
using expected = std::expected<T, E>;

#else

// Exception type for bad expected access (mimics C++23 std::bad_expected_access)
template <class E>
class bad_expected_access : public std::exception {
 public:
  explicit bad_expected_access(E e) : err_(std::move(e)) {}

  auto error() const& -> E const& { return err_; }
  auto error() & -> E& { return err_; }
  auto error() const&& -> E const&& { return std::move(err_); }
  auto error() && -> E&& { return std::move(err_); }

  const char* what() const noexcept override { return "bad expected access"; }

 private:
  E err_;
};

template <class E>
class unexpected {
 public:
  constexpr explicit unexpected(E const& e) : error_(e) {}
  constexpr explicit unexpected(E&& e) : error_(std::move(e)) {}

  constexpr E const& error() const& noexcept { return error_; }
  constexpr E& error() & noexcept { return error_; }
  constexpr E const&& error() const&& noexcept { return std::move(error_); }
  constexpr E&& error() && noexcept { return std::move(error_); }

 private:
  E error_;
};

template <class E>
unexpected(E) -> unexpected<E>;

template <class E>
constexpr bool operator==(unexpected<E> const& lhs, unexpected<E> const& rhs) {
  return lhs.error() == rhs.error();
}

template <class E>
constexpr bool operator!=(unexpected<E> const& lhs, unexpected<E> const& rhs) {
  return !(lhs == rhs);
}

struct unexpect_t {
  explicit unexpect_t() = default;
};
inline constexpr unexpect_t unexpect{};

template <class T, class E>
class expected {
 public:
  using value_type = T;
  using error_type = E;

  constexpr expected() : storage_(std::in_place_index<0>) {}

  constexpr expected(T const& value) : storage_(std::in_place_index<0>, value) {}
  constexpr expected(T&& value) : storage_(std::in_place_index<0>, std::move(value)) {}

  constexpr expected(unexpected<E> const& u) : storage_(std::in_place_index<1>, u.error()) {}
  constexpr expected(unexpected<E>&& u) : storage_(std::in_place_index<1>, std::move(u).error()) {}

  template <class G, typename = std::enable_if_t<std::is_convertible_v<G const&, E>>>
  constexpr expected(unexpected<G> const& u) : storage_(std::in_place_index<1>, E(u.error())) {}

  template <class G, typename = std::enable_if_t<std::is_convertible_v<G&&, E>>>
  constexpr expected(unexpected<G>&& u)
      : storage_(std::in_place_index<1>, E(std::move(u).error())) {}

  template <class... Args>
  constexpr explicit expected(std::in_place_t, Args&&... args)
      : storage_(std::in_place_index<0>, std::forward<Args>(args)...) {}

  template <class... Args>
  constexpr explicit expected(unexpect_t, Args&&... args)
      : storage_(std::in_place_index<1>, std::forward<Args>(args)...) {}

  constexpr bool has_value() const noexcept { return storage_.index() == 0; }
  constexpr explicit operator bool() const noexcept { return has_value(); }

  constexpr T& value() & {
    if (!has_value()) {
      throw_bad_expected_access();
    }
    return std::get<0>(storage_);
  }

  constexpr T const& value() const& {
    if (!has_value()) {
      throw_bad_expected_access();
    }
    return std::get<0>(storage_);
  }

  constexpr T&& value() && {
    if (!has_value()) {
      throw_bad_expected_access();
    }
    return std::move(std::get<0>(storage_));
  }

  constexpr T const&& value() const&& {
    if (!has_value()) {
      throw_bad_expected_access();
    }
    return std::move(std::get<0>(storage_));
  }

  constexpr E& error() & noexcept { return std::get<1>(storage_); }
  constexpr E const& error() const& noexcept { return std::get<1>(storage_); }
  constexpr E&& error() && noexcept { return std::move(std::get<1>(storage_)); }
  constexpr E const&& error() const&& noexcept { return std::move(std::get<1>(storage_)); }

  constexpr T& operator*() & noexcept { return std::get<0>(storage_); }
  constexpr T const& operator*() const& noexcept { return std::get<0>(storage_); }
  constexpr T&& operator*() && noexcept { return std::move(std::get<0>(storage_)); }
  constexpr T const&& operator*() const&& noexcept { return std::move(std::get<0>(storage_)); }

  constexpr T* operator->() noexcept { return &std::get<0>(storage_); }
  constexpr T const* operator->() const noexcept { return &std::get<0>(storage_); }

  template <class U>
  constexpr T value_or(U&& default_value) const& {
    return has_value() ? **this : static_cast<T>(std::forward<U>(default_value));
  }

  template <class U>
  constexpr T value_or(U&& default_value) && {
    return has_value() ? std::move(**this) : static_cast<T>(std::forward<U>(default_value));
  }

  template <class F>
  constexpr auto and_then(F&& f) & {
    using U = std::invoke_result_t<F, T&>;
    if (has_value()) {
      return std::invoke(std::forward<F>(f), **this);
    } else {
      return U(unexpected<E>(error()));
    }
  }

  template <class F>
  constexpr auto and_then(F&& f) const& {
    using U = std::invoke_result_t<F, T const&>;
    if (has_value()) {
      return std::invoke(std::forward<F>(f), **this);
    } else {
      return U(unexpected<E>(error()));
    }
  }

  template <class F>
  constexpr auto and_then(F&& f) && {
    using U = std::invoke_result_t<F, T&&>;
    if (has_value()) {
      return std::invoke(std::forward<F>(f), std::move(**this));
    } else {
      return U(unexpected<E>(std::move(error())));
    }
  }

  template <class F>
  constexpr auto and_then(F&& f) const&& {
    using U = std::invoke_result_t<F, T const&&>;
    if (has_value()) {
      return std::invoke(std::forward<F>(f), std::move(**this));
    } else {
      return U(unexpected<E>(std::move(error())));
    }
  }

  template <class F>
  constexpr auto transform(F&& f) & {
    using U = std::remove_cv_t<std::invoke_result_t<F, T&>>;
    if (has_value()) {
      if constexpr (std::is_void_v<U>) {
        std::invoke(std::forward<F>(f), **this);
        return expected<void, E>();
      } else {
        return expected<U, E>(std::in_place, std::invoke(std::forward<F>(f), **this));
      }
    } else {
      return expected<U, E>(unexpected<E>(error()));
    }
  }

  template <class F>
  constexpr auto transform(F&& f) const& {
    using U = std::remove_cv_t<std::invoke_result_t<F, T const&>>;
    if (has_value()) {
      if constexpr (std::is_void_v<U>) {
        std::invoke(std::forward<F>(f), **this);
        return expected<void, E>();
      } else {
        return expected<U, E>(std::in_place, std::invoke(std::forward<F>(f), **this));
      }
    } else {
      return expected<U, E>(unexpected<E>(error()));
    }
  }

  template <class F>
  constexpr auto transform(F&& f) && {
    using U = std::remove_cv_t<std::invoke_result_t<F, T&&>>;
    if (has_value()) {
      if constexpr (std::is_void_v<U>) {
        std::invoke(std::forward<F>(f), std::move(**this));
        return expected<void, E>();
      } else {
        return expected<U, E>(std::in_place, std::invoke(std::forward<F>(f), std::move(**this)));
      }
    } else {
      return expected<U, E>(unexpected<E>(std::move(error())));
    }
  }

  template <class F>
  constexpr auto transform(F&& f) const&& {
    using U = std::remove_cv_t<std::invoke_result_t<F, T const&&>>;
    if (has_value()) {
      if constexpr (std::is_void_v<U>) {
        std::invoke(std::forward<F>(f), std::move(**this));
        return expected<void, E>();
      } else {
        return expected<U, E>(std::in_place, std::invoke(std::forward<F>(f), std::move(**this)));
      }
    } else {
      return expected<U, E>(unexpected<E>(std::move(error())));
    }
  }

  template <class F>
  constexpr auto or_else(F&& f) & {
    using G = std::remove_cvref_t<std::invoke_result_t<F, E&>>;
    if (has_value()) {
      return G(std::in_place, **this);
    } else {
      return std::invoke(std::forward<F>(f), error());
    }
  }

  template <class F>
  constexpr auto or_else(F&& f) const& {
    using G = std::remove_cvref_t<std::invoke_result_t<F, E const&>>;
    if (has_value()) {
      return G(std::in_place, **this);
    } else {
      return std::invoke(std::forward<F>(f), error());
    }
  }

  template <class F>
  constexpr auto or_else(F&& f) && {
    using G = std::remove_cvref_t<std::invoke_result_t<F, E&&>>;
    if (has_value()) {
      return G(std::in_place, std::move(**this));
    } else {
      return std::invoke(std::forward<F>(f), std::move(error()));
    }
  }

  template <class F>
  constexpr auto or_else(F&& f) const&& {
    using G = std::remove_cvref_t<std::invoke_result_t<F, E const&&>>;
    if (has_value()) {
      return G(std::in_place, std::move(**this));
    } else {
      return std::invoke(std::forward<F>(f), std::move(error()));
    }
  }

 private:
  std::variant<T, E> storage_;

  [[noreturn]] void throw_bad_expected_access() const { throw bad_expected_access<E>(error()); }
};

template <class T, class E>
constexpr bool operator==(expected<T, E> const& lhs, expected<T, E> const& rhs) {
  if (lhs.has_value() != rhs.has_value()) {
    return false;
  }
  if (lhs.has_value()) {
    return *lhs == *rhs;
  }
  return lhs.error() == rhs.error();
}

template <class T, class E>
constexpr bool operator!=(expected<T, E> const& lhs, expected<T, E> const& rhs) {
  return !(lhs == rhs);
}

template <class T, class E>
constexpr bool operator==(expected<T, E> const& x, T const& v) {
  return x.has_value() && *x == v;
}

template <class T, class E>
constexpr bool operator==(T const& v, expected<T, E> const& x) {
  return x == v;
}

template <class T, class E>
constexpr bool operator!=(expected<T, E> const& x, T const& v) {
  return !(x == v);
}

template <class T, class E>
constexpr bool operator!=(T const& v, expected<T, E> const& x) {
  return !(x == v);
}

template <class T, class E>
constexpr bool operator==(expected<T, E> const& x, unexpected<E> const& e) {
  return !x.has_value() && x.error() == e.error();
}

template <class T, class E>
constexpr bool operator==(unexpected<E> const& e, expected<T, E> const& x) {
  return x == e;
}

template <class T, class E>
constexpr bool operator!=(expected<T, E> const& x, unexpected<E> const& e) {
  return !(x == e);
}

template <class T, class E>
constexpr bool operator!=(unexpected<E> const& e, expected<T, E> const& x) {
  return !(x == e);
}

template <class T, class E, class G>
constexpr bool operator==(expected<T, E> const& x, unexpected<G> const& e) {
  return !x.has_value() && x.error() == e.error();
}

template <class T, class E, class G>
constexpr bool operator==(unexpected<G> const& e, expected<T, E> const& x) {
  return x == e;
}

template <class T, class E, class G>
constexpr bool operator!=(expected<T, E> const& x, unexpected<G> const& e) {
  return !(x == e);
}

template <class T, class E, class G>
constexpr bool operator!=(unexpected<G> const& e, expected<T, E> const& x) {
  return !(x == e);
}

/// Specialization for void value type
template <class E>
class expected<void, E> {
 public:
  using value_type = void;
  using error_type = E;

  constexpr expected() : storage_(std::in_place_index<0>) {}

  constexpr expected(unexpected<E> const& u) : storage_(std::in_place_index<1>, u.error()) {}
  constexpr expected(unexpected<E>&& u) : storage_(std::in_place_index<1>, std::move(u).error()) {}

  template <class G, typename = std::enable_if_t<std::is_convertible_v<G const&, E>>>
  constexpr expected(unexpected<G> const& u) : storage_(std::in_place_index<1>, E(u.error())) {}

  template <class G, typename = std::enable_if_t<std::is_convertible_v<G&&, E>>>
  constexpr expected(unexpected<G>&& u)
      : storage_(std::in_place_index<1>, E(std::move(u).error())) {}

  template <class... Args>
  constexpr explicit expected(unexpect_t, Args&&... args)
      : storage_(std::in_place_index<1>, std::forward<Args>(args)...) {}

  constexpr bool has_value() const noexcept { return storage_.index() == 0; }
  constexpr explicit operator bool() const noexcept { return has_value(); }

  constexpr void value() const& {
    if (!has_value()) {
      throw_bad_expected_access();
    }
  }

  constexpr void value() && {
    if (!has_value()) {
      throw_bad_expected_access();
    }
  }

  constexpr E& error() & noexcept { return std::get<1>(storage_); }
  constexpr E const& error() const& noexcept { return std::get<1>(storage_); }
  constexpr E&& error() && noexcept { return std::move(std::get<1>(storage_)); }
  constexpr E const&& error() const&& noexcept { return std::move(std::get<1>(storage_)); }

  constexpr void operator*() const noexcept {}

  template <class F>
  constexpr auto and_then(F&& f) & {
    using U = std::invoke_result_t<F>;
    if (has_value()) {
      return std::invoke(std::forward<F>(f));
    } else {
      return U(unexpected<E>(error()));
    }
  }

  template <class F>
  constexpr auto and_then(F&& f) const& {
    using U = std::invoke_result_t<F>;
    if (has_value()) {
      return std::invoke(std::forward<F>(f));
    } else {
      return U(unexpected<E>(error()));
    }
  }

  template <class F>
  constexpr auto and_then(F&& f) && {
    using U = std::invoke_result_t<F>;
    if (has_value()) {
      return std::invoke(std::forward<F>(f));
    } else {
      return U(unexpected<E>(std::move(error())));
    }
  }

  template <class F>
  constexpr auto transform(F&& f) & {
    using U = std::remove_cv_t<std::invoke_result_t<F>>;
    if (has_value()) {
      if constexpr (std::is_void_v<U>) {
        std::invoke(std::forward<F>(f));
        return expected<void, E>();
      } else {
        return expected<U, E>(std::in_place, std::invoke(std::forward<F>(f)));
      }
    } else {
      return expected<U, E>(unexpected<E>(error()));
    }
  }

  template <class F>
  constexpr auto transform(F&& f) const& {
    using U = std::remove_cv_t<std::invoke_result_t<F>>;
    if (has_value()) {
      if constexpr (std::is_void_v<U>) {
        std::invoke(std::forward<F>(f));
        return expected<void, E>();
      } else {
        return expected<U, E>(std::in_place, std::invoke(std::forward<F>(f)));
      }
    } else {
      return expected<U, E>(unexpected<E>(error()));
    }
  }

  template <class F>
  constexpr auto transform(F&& f) && {
    using U = std::remove_cv_t<std::invoke_result_t<F>>;
    if (has_value()) {
      if constexpr (std::is_void_v<U>) {
        std::invoke(std::forward<F>(f));
        return expected<void, E>();
      } else {
        return expected<U, E>(std::in_place, std::invoke(std::forward<F>(f)));
      }
    } else {
      return expected<U, E>(unexpected<E>(std::move(error())));
    }
  }

  template <class F>
  constexpr auto transform(F&& f) const&& {
    using U = std::remove_cv_t<std::invoke_result_t<F>>;
    if (has_value()) {
      if constexpr (std::is_void_v<U>) {
        std::invoke(std::forward<F>(f));
        return expected<void, E>();
      } else {
        return expected<U, E>(std::in_place, std::invoke(std::forward<F>(f)));
      }
    } else {
      return expected<U, E>(unexpected<E>(std::move(error())));
    }
  }

  template <class F>
  constexpr auto or_else(F&& f) & {
    using G = std::remove_cvref_t<std::invoke_result_t<F, E&>>;
    if (has_value()) {
      return G();
    } else {
      return std::invoke(std::forward<F>(f), error());
    }
  }

 private:
  std::variant<std::monostate, E> storage_;

  [[noreturn]] void throw_bad_expected_access() const { throw bad_expected_access<E>(error()); }
};

#endif

}  // namespace iocoro

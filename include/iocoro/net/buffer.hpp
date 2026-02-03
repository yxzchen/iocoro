#pragma once

#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace iocoro::net {

namespace detail {

template <class T>
inline constexpr bool is_byte_like_v =
  std::is_same_v<std::remove_cv_t<T>, std::byte> ||
  std::is_same_v<std::remove_cv_t<T>, unsigned char> || std::is_same_v<std::remove_cv_t<T>, char>;

template <class T>
inline constexpr bool is_vector_bool_v = false;

template <class Alloc>
inline constexpr bool is_vector_bool_v<std::vector<bool, Alloc>> = true;

}  // namespace detail

/// A read-only buffer (Boost.Asio-style).
///
/// A buffer is a (pointer, size_in_bytes) pair and does not own the memory.
class const_buffer {
 public:
  constexpr const_buffer() noexcept = default;
  constexpr const_buffer(void const* data, std::size_t size) noexcept
      : data_(static_cast<std::byte const*>(data)), size_(size) {}

  constexpr const_buffer(std::span<std::byte const> s) noexcept
      : data_(s.data()), size_(s.size()) {}

  /// Construct a non-modifiable buffer from a modifiable one (implicit, Asio-style).
  constexpr const_buffer(class mutable_buffer const& b) noexcept;

  /// Get a pointer to the beginning of the memory range.
  constexpr auto data() const noexcept -> void const* { return data_; }
  constexpr auto size() const noexcept -> std::size_t { return size_; }
  constexpr auto empty() const noexcept -> bool { return size_ == 0; }

  /// Advance the buffer by `n` bytes (clamped).
  constexpr auto operator+=(std::size_t n) noexcept -> const_buffer& {
    if (n > size_) {
      n = size_;
    }
    data_ += n;
    size_ -= n;
    return *this;
  }

  constexpr auto as_span() const noexcept -> std::span<std::byte const> { return {data_, size_}; }

 private:
  std::byte const* data_ = nullptr;
  std::size_t size_ = 0;
};

/// A writable buffer (Boost.Asio-style).
///
/// A buffer is a (pointer, size_in_bytes) pair and does not own the memory.
class mutable_buffer {
 public:
  constexpr mutable_buffer() noexcept = default;
  constexpr mutable_buffer(void* data, std::size_t size) noexcept
      : data_(static_cast<std::byte*>(data)), size_(size) {}

  constexpr mutable_buffer(std::span<std::byte> s) noexcept : data_(s.data()), size_(s.size()) {}

  /// Get a pointer to the beginning of the memory range.
  constexpr auto data() const noexcept -> void* { return data_; }
  constexpr auto size() const noexcept -> std::size_t { return size_; }
  constexpr auto empty() const noexcept -> bool { return size_ == 0; }

  /// Advance the buffer by `n` bytes (clamped).
  constexpr auto operator+=(std::size_t n) noexcept -> mutable_buffer& {
    if (n > size_) {
      n = size_;
    }
    data_ += n;
    size_ -= n;
    return *this;
  }

  constexpr auto as_span() const noexcept -> std::span<std::byte> { return {data_, size_}; }

 private:
  std::byte* data_ = nullptr;
  std::size_t size_ = 0;
};

inline constexpr const_buffer::const_buffer(mutable_buffer const& b) noexcept
    : data_(static_cast<std::byte const*>(b.as_span().data())), size_(b.as_span().size()) {}

// Buffer arithmetic (Boost.Asio-style).
inline constexpr auto operator+(const_buffer b, std::size_t n) noexcept -> const_buffer {
  b += n;
  return b;
}

inline constexpr auto operator+(mutable_buffer b, std::size_t n) noexcept -> mutable_buffer {
  b += n;
  return b;
}

// Convenience helpers (Boost.Asio-style).
inline constexpr auto buffer_size(const_buffer b) noexcept -> std::size_t {
  return b.size();
}
inline constexpr auto buffer_size(mutable_buffer b) noexcept -> std::size_t {
  return b.size();
}

template <class T>
inline auto buffer_cast(const_buffer b) noexcept -> T {
  return static_cast<T>(b.data());
}

template <class T>
inline auto buffer_cast(mutable_buffer b) noexcept -> T {
  return static_cast<T>(b.data());
}

// ---- buffer(...) helpers (avoid user-side reinterpret_cast) ----

// Existing buffers.
inline constexpr auto buffer(mutable_buffer b) noexcept -> mutable_buffer {
  return b;
}
inline constexpr auto buffer(mutable_buffer b, std::size_t max_size_in_bytes) noexcept
  -> mutable_buffer {
  if (max_size_in_bytes < b.size()) {
    return mutable_buffer{b.as_span().data(), max_size_in_bytes};
  }
  return b;
}

inline constexpr auto buffer(const_buffer b) noexcept -> const_buffer {
  return b;
}
inline constexpr auto buffer(const_buffer b, std::size_t max_size_in_bytes) noexcept
  -> const_buffer {
  if (max_size_in_bytes < b.size()) {
    return const_buffer{b.as_span().data(), max_size_in_bytes};
  }
  return b;
}

// Raw memory.
inline constexpr auto buffer(void* data, std::size_t size_in_bytes) noexcept -> mutable_buffer {
  return mutable_buffer{data, size_in_bytes};
}

inline constexpr auto buffer(void const* data, std::size_t size_in_bytes) noexcept -> const_buffer {
  return const_buffer{data, size_in_bytes};
}

// std::span
inline constexpr auto buffer(std::span<std::byte> s) noexcept -> mutable_buffer {
  return mutable_buffer{s};
}
inline constexpr auto buffer(std::span<std::byte> s, std::size_t max_size_in_bytes) noexcept
  -> mutable_buffer {
  return buffer(mutable_buffer{s}, max_size_in_bytes);
}

inline constexpr auto buffer(std::span<std::byte const> s) noexcept -> const_buffer {
  return const_buffer{s};
}
inline constexpr auto buffer(std::span<std::byte const> s, std::size_t max_size_in_bytes) noexcept
  -> const_buffer {
  return buffer(const_buffer{s}, max_size_in_bytes);
}

template <class T>
  requires(!std::is_const_v<T>)
inline constexpr auto buffer(std::span<T> s) noexcept -> mutable_buffer {
  auto b = std::as_writable_bytes(s);
  return mutable_buffer{b};
}

template <class T>
  requires(!std::is_const_v<T>)
inline constexpr auto buffer(std::span<T> s, std::size_t max_size_in_bytes) noexcept
  -> mutable_buffer {
  return buffer(buffer(s), max_size_in_bytes);
}

template <class T>
inline constexpr auto buffer(std::span<T const> s) noexcept -> const_buffer {
  auto b = std::as_bytes(s);
  return const_buffer{b};
}

template <class T>
inline constexpr auto buffer(std::span<T const> s, std::size_t max_size_in_bytes) noexcept
  -> const_buffer {
  return buffer(buffer(s), max_size_in_bytes);
}

// C arrays.
template <class T, std::size_t N>
  requires(!std::is_const_v<T>)
inline constexpr auto buffer(T (&data)[N]) noexcept -> mutable_buffer {
  static_assert(std::is_trivially_copyable_v<T>,
                "buffer(array): element type must be trivially copyable");
  auto sp = std::span<T>(data);
  return buffer(sp);
}

template <class T, std::size_t N>
inline constexpr auto buffer(T const (&data)[N]) noexcept -> const_buffer {
  static_assert(std::is_trivially_copyable_v<T>,
                "buffer(array): element type must be trivially copyable");
  auto sp = std::span<T const>(data);
  return buffer(sp);
}

template <class T, std::size_t N>
inline constexpr auto buffer(T (&data)[N], std::size_t max_size_in_bytes) noexcept
  -> mutable_buffer {
  return buffer(buffer(data), max_size_in_bytes);
}

template <class T, std::size_t N>
inline constexpr auto buffer(T const (&data)[N], std::size_t max_size_in_bytes) noexcept
  -> const_buffer {
  return buffer(buffer(data), max_size_in_bytes);
}

// std::array.
template <class T, std::size_t N>
  requires(!std::is_const_v<T>)
inline constexpr auto buffer(std::array<T, N>& a) noexcept -> mutable_buffer {
  static_assert(std::is_trivially_copyable_v<T>,
                "buffer(std::array): element type must be trivially copyable");
  return buffer(std::span<T>(a));
}

template <class T, std::size_t N>
inline constexpr auto buffer(std::array<T, N> const& a) noexcept -> const_buffer {
  static_assert(std::is_trivially_copyable_v<T>,
                "buffer(std::array): element type must be trivially copyable");
  return buffer(std::span<T const>(a));
}

template <class T, std::size_t N>
inline constexpr auto buffer(std::array<T, N>& a, std::size_t max_size_in_bytes) noexcept
  -> mutable_buffer {
  return buffer(buffer(a), max_size_in_bytes);
}

template <class T, std::size_t N>
inline constexpr auto buffer(std::array<T, N> const& a, std::size_t max_size_in_bytes) noexcept
  -> const_buffer {
  return buffer(buffer(a), max_size_in_bytes);
}

// std::vector.
template <class T, class Alloc>
  requires(!detail::is_vector_bool_v<std::vector<T, Alloc>>)
inline auto buffer(std::vector<T, Alloc>& v) noexcept -> mutable_buffer {
  static_assert(std::is_trivially_copyable_v<T>,
                "buffer(std::vector): element type must be trivially copyable");
  return buffer(std::span<T>(v.data(), v.size()));
}

template <class T, class Alloc>
  requires(!detail::is_vector_bool_v<std::vector<T, Alloc>>)
inline auto buffer(std::vector<T, Alloc> const& v) noexcept -> const_buffer {
  static_assert(std::is_trivially_copyable_v<T>,
                "buffer(std::vector): element type must be trivially copyable");
  return buffer(std::span<T const>(v.data(), v.size()));
}

template <class T, class Alloc>
  requires(!detail::is_vector_bool_v<std::vector<T, Alloc>>)
inline auto buffer(std::vector<T, Alloc>& v, std::size_t max_size_in_bytes) noexcept
  -> mutable_buffer {
  return buffer(buffer(v), max_size_in_bytes);
}

template <class T, class Alloc>
  requires(!detail::is_vector_bool_v<std::vector<T, Alloc>>)
inline auto buffer(std::vector<T, Alloc> const& v, std::size_t max_size_in_bytes) noexcept
  -> const_buffer {
  return buffer(buffer(v), max_size_in_bytes);
}

// std::basic_string / std::basic_string_view.
template <class CharT, class Traits, class Alloc>
inline auto buffer(std::basic_string<CharT, Traits, Alloc>& s) noexcept -> mutable_buffer {
  static_assert(std::is_trivially_copyable_v<CharT>,
                "buffer(string): character type must be trivially copyable");
  auto sp = std::span<CharT>(s.data(), s.size());
  auto b = std::as_writable_bytes(sp);
  return mutable_buffer{b};
}

template <class CharT, class Traits, class Alloc>
inline auto buffer(std::basic_string<CharT, Traits, Alloc> const& s) noexcept -> const_buffer {
  static_assert(std::is_trivially_copyable_v<CharT>,
                "buffer(string): character type must be trivially copyable");
  auto sp = std::span<CharT const>(s.data(), s.size());
  auto b = std::as_bytes(sp);
  return const_buffer{b};
}

template <class CharT, class Traits, class Alloc>
inline auto buffer(std::basic_string<CharT, Traits, Alloc>& s,
                   std::size_t max_size_in_bytes) noexcept -> mutable_buffer {
  return buffer(buffer(s), max_size_in_bytes);
}

template <class CharT, class Traits, class Alloc>
inline auto buffer(std::basic_string<CharT, Traits, Alloc> const& s,
                   std::size_t max_size_in_bytes) noexcept -> const_buffer {
  return buffer(buffer(s), max_size_in_bytes);
}

template <class CharT, class Traits>
inline auto buffer(std::basic_string_view<CharT, Traits> s) noexcept -> const_buffer {
  static_assert(std::is_trivially_copyable_v<CharT>,
                "buffer(string_view): character type must be trivially copyable");
  auto sp = std::span<CharT const>(s.data(), s.size());
  auto b = std::as_bytes(sp);
  return const_buffer{b};
}

}  // namespace iocoro::net

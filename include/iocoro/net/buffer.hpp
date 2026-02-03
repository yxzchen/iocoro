#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace iocoro::net {

/// A read-only byte buffer view (Asio-style).
class const_buffer {
 public:
  constexpr const_buffer() noexcept = default;
  constexpr const_buffer(std::byte const* data, std::size_t size) noexcept
      : data_(data), size_(size) {}

  constexpr auto data() const noexcept -> std::byte const* { return data_; }
  constexpr auto size() const noexcept -> std::size_t { return size_; }
  constexpr auto empty() const noexcept -> bool { return size_ == 0; }

  constexpr auto as_span() const noexcept -> std::span<std::byte const> { return {data_, size_}; }

  constexpr auto first(std::size_t n) const noexcept -> const_buffer {
    if (n > size_) {
      n = size_;
    }
    return const_buffer{data_, n};
  }

  constexpr auto subspan(std::size_t offset) const noexcept -> const_buffer {
    if (offset > size_) {
      offset = size_;
    }
    return const_buffer{data_ + offset, size_ - offset};
  }

  constexpr auto subspan(std::size_t offset, std::size_t count) const noexcept -> const_buffer {
    return subspan(offset).first(count);
  }

 private:
  std::byte const* data_ = nullptr;
  std::size_t size_ = 0;
};

/// A writable byte buffer view (Asio-style).
class mutable_buffer {
 public:
  constexpr mutable_buffer() noexcept = default;
  constexpr mutable_buffer(std::byte* data, std::size_t size) noexcept : data_(data), size_(size) {}

  constexpr auto data() const noexcept -> std::byte* { return data_; }
  constexpr auto size() const noexcept -> std::size_t { return size_; }
  constexpr auto empty() const noexcept -> bool { return size_ == 0; }

  /// Implicit conversion to a read-only view (Asio-style).
  constexpr operator const_buffer() const noexcept { return const_buffer{data_, size_}; }

  constexpr auto as_span() const noexcept -> std::span<std::byte> { return {data_, size_}; }

  constexpr auto first(std::size_t n) const noexcept -> mutable_buffer {
    if (n > size_) {
      n = size_;
    }
    return mutable_buffer{data_, n};
  }

  constexpr auto subspan(std::size_t offset) const noexcept -> mutable_buffer {
    if (offset > size_) {
      offset = size_;
    }
    return mutable_buffer{data_ + offset, size_ - offset};
  }

  constexpr auto subspan(std::size_t offset, std::size_t count) const noexcept -> mutable_buffer {
    return subspan(offset).first(count);
  }

 private:
  std::byte* data_ = nullptr;
  std::size_t size_ = 0;
};

// ---- buffer(...) helpers (avoid user-side reinterpret_cast) ----

constexpr auto buffer(std::span<std::byte> s) noexcept -> mutable_buffer {
  return mutable_buffer{s.data(), s.size()};
}

constexpr auto buffer(std::span<std::byte const> s) noexcept -> const_buffer {
  return const_buffer{s.data(), s.size()};
}

template <class T>
  requires(!std::is_const_v<T>)
constexpr auto buffer(std::span<T> s) noexcept -> mutable_buffer {
  auto b = std::as_writable_bytes(s);
  return mutable_buffer{b.data(), b.size()};
}

template <class T>
constexpr auto buffer(std::span<T const> s) noexcept -> const_buffer {
  auto b = std::as_bytes(s);
  return const_buffer{b.data(), b.size()};
}

inline auto buffer(std::string& s) noexcept -> mutable_buffer {
  auto sp = std::span<char>(s.data(), s.size());
  auto b = std::as_writable_bytes(sp);
  return mutable_buffer{b.data(), b.size()};
}

inline auto buffer(std::string const& s) noexcept -> const_buffer {
  auto sp = std::span<char const>(s.data(), s.size());
  auto b = std::as_bytes(sp);
  return const_buffer{b.data(), b.size()};
}

inline auto buffer(std::string_view s) noexcept -> const_buffer {
  auto sp = std::span<char const>(s.data(), s.size());
  auto b = std::as_bytes(sp);
  return const_buffer{b.data(), b.size()};
}

inline auto buffer(std::vector<std::byte>& v) noexcept -> mutable_buffer {
  return mutable_buffer{v.data(), v.size()};
}

inline auto buffer(std::vector<std::byte> const& v) noexcept -> const_buffer {
  return const_buffer{v.data(), v.size()};
}

}  // namespace iocoro::net

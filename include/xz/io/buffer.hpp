#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace xz::io {

/// CRTP base for buffer implementations
template <typename Derived>
class buffer_base {
 protected:
  std::size_t read_pos_ = 0;
  std::size_t write_pos_ = 0;

  auto derived() noexcept -> Derived& { return static_cast<Derived&>(*this); }
  auto derived() const noexcept -> Derived const& { return static_cast<Derived const&>(*this); }

 public:
  [[nodiscard]] auto size() const noexcept -> std::size_t { return write_pos_ - read_pos_; }
  [[nodiscard]] auto empty() const noexcept -> bool { return read_pos_ == write_pos_; }

  [[nodiscard]] auto data() const noexcept -> char const* {
    return derived().storage_data() + read_pos_;
  }

  [[nodiscard]] auto readable() const noexcept -> std::span<char const> {
    return {derived().storage_data() + read_pos_, size()};
  }

  [[nodiscard]] auto view() const noexcept -> std::string_view {
    return {derived().storage_data() + read_pos_, size()};
  }

  void consume(std::size_t n) noexcept {
    read_pos_ = std::min(read_pos_ + n, write_pos_);
    if (read_pos_ == write_pos_) {
      read_pos_ = write_pos_ = 0;
    }
    if (read_pos_ > (derived().capacity() >> 1)) {
      compact();
    }
  }

  void commit(std::size_t n) noexcept { write_pos_ += n; }

  void append(std::span<char const> data) {
    auto buf = derived().prepare(data.size());
    std::memcpy(buf.data(), data.data(), data.size());
    commit(data.size());
  }

  void append(std::string_view str) { append(std::span<char const>{str.data(), str.size()}); }

  void clear() noexcept {
    read_pos_ = 0;
    write_pos_ = 0;
  }

  void compact() noexcept {
    if (read_pos_ == 0) return;

    auto const sz = size();
    if (sz > 0) {
      std::memmove(derived().storage_data(), derived().storage_data() + read_pos_, sz);
    }
    read_pos_ = 0;
    write_pos_ = sz;
  }
};

/// A dynamic buffer that efficiently manages read/write positions
class dynamic_buffer : public buffer_base<dynamic_buffer> {
 public:
  explicit dynamic_buffer(std::size_t initial_capacity = 8192) { storage_.reserve(initial_capacity); }

  [[nodiscard]] auto capacity() const noexcept -> std::size_t { return storage_.capacity(); }

  void reserve(std::size_t n) {
    if (n > capacity()) {
      storage_.reserve(n);
    }
  }

  [[nodiscard]] auto prepare(std::size_t n) -> std::span<char> {
    if (write_pos_ + n > storage_.capacity()) {
      compact();

      if (write_pos_ + n > storage_.capacity()) {
        auto new_cap = std::max(storage_.capacity() * 2, write_pos_ + n);
        storage_.reserve(new_cap);
      }
    }

    if (write_pos_ + n > storage_.size()) {
      storage_.resize(write_pos_ + n);
    }

    return {storage_.data() + write_pos_, n};
  }

  auto storage_data() noexcept -> char* { return storage_.data(); }
  auto storage_data() const noexcept -> char const* { return storage_.data(); }

 private:
  friend class buffer_base<dynamic_buffer>;
  std::vector<char> storage_;
};

/// A fixed-size buffer with static allocation
template <std::size_t N>
class static_buffer : public buffer_base<static_buffer<N>> {
 public:
  [[nodiscard]] constexpr auto capacity() const noexcept -> std::size_t { return N; }

  [[nodiscard]] auto prepare(std::size_t n) -> std::span<char> {
    if (this->write_pos_ + n > N) {
      this->compact();
      if (this->write_pos_ + n > N) {
        throw std::length_error("static_buffer overflow");
      }
    }
    return {storage_.data() + this->write_pos_, n};
  }

  auto storage_data() noexcept -> char* { return storage_.data(); }
  auto storage_data() const noexcept -> char const* { return storage_.data(); }

 private:
  friend class buffer_base<static_buffer<N>>;
  std::array<char, N> storage_;
};

}  // namespace xz::io

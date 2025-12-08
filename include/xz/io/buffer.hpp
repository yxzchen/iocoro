#pragma once

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace xz::io {

/// A dynamic buffer that efficiently manages read/write positions
class dynamic_buffer {
 public:
  explicit dynamic_buffer(std::size_t initial_capacity = 8192) {
    storage_.reserve(initial_capacity);
  }

  // Capacity management
  auto capacity() const noexcept -> std::size_t { return storage_.capacity(); }
  auto size() const noexcept -> std::size_t { return write_pos_ - read_pos_; }
  auto empty() const noexcept -> bool { return read_pos_ == write_pos_; }

  void reserve(std::size_t n) {
    if (n > capacity()) {
      storage_.reserve(n);
    }
  }

  // Reading interface
  auto data() const noexcept -> char const* { return storage_.data() + read_pos_; }
  auto readable() const noexcept -> std::span<char const> {
    return {storage_.data() + read_pos_, size()};
  }

  auto view() const noexcept -> std::string_view {
    return {storage_.data() + read_pos_, size()};
  }

  void consume(std::size_t n) noexcept {
    read_pos_ = std::min(read_pos_ + n, write_pos_);
    maybe_compact();
  }

  // Writing interface
  auto prepare(std::size_t n) -> std::span<char> {
    ensure_space(n);
    if (write_pos_ + n > storage_.size()) {
      storage_.resize(write_pos_ + n);
    }
    return {storage_.data() + write_pos_, n};
  }

  void commit(std::size_t n) noexcept {
    write_pos_ = std::min(write_pos_ + n, storage_.size());
  }

  // Convenience methods
  void append(std::span<char const> data) {
    auto buf = prepare(data.size());
    std::memcpy(buf.data(), data.data(), data.size());
    commit(data.size());
  }

  void append(std::string_view str) {
    append(std::span<char const>{str.data(), str.size()});
  }

  void clear() noexcept {
    read_pos_ = 0;
    write_pos_ = 0;
  }

  // Explicit compact
  void compact() {
    if (read_pos_ == 0) return;

    auto const sz = size();
    if (sz > 0) {
      std::memmove(storage_.data(), storage_.data() + read_pos_, sz);
    }
    read_pos_ = 0;
    write_pos_ = sz;
  }

 private:
  void ensure_space(std::size_t n) {
    if (write_pos_ + n <= storage_.capacity()) {
      return;
    }

    compact();

    if (write_pos_ + n > storage_.capacity()) {
      auto new_cap = std::max(storage_.capacity() * 2, write_pos_ + n);
      storage_.reserve(new_cap);
    }
  }

  void maybe_compact() {
    // Auto-compact if we've consumed more than half the capacity
    if (read_pos_ > storage_.capacity() / 2) {
      compact();
    }
  }

  std::vector<char> storage_;
  std::size_t read_pos_ = 0;
  std::size_t write_pos_ = 0;
};

/// A fixed-size buffer with static allocation
template <std::size_t N>
class static_buffer {
 public:
  auto capacity() const noexcept -> std::size_t { return N; }
  auto size() const noexcept -> std::size_t { return write_pos_ - read_pos_; }
  auto empty() const noexcept -> bool { return read_pos_ == write_pos_; }

  auto data() const noexcept -> char const* { return storage_.data() + read_pos_; }
  auto readable() const noexcept -> std::span<char const> {
    return {storage_.data() + read_pos_, size()};
  }

  void consume(std::size_t n) noexcept {
    read_pos_ = std::min(read_pos_ + n, write_pos_);
    if (read_pos_ == write_pos_) {
      read_pos_ = write_pos_ = 0;
    }
  }

  auto prepare(std::size_t n) -> std::span<char> {
    if (write_pos_ + n > N) {
      compact();
      if (write_pos_ + n > N) {
        throw std::length_error("static_buffer overflow");
      }
    }
    return {storage_.data() + write_pos_, n};
  }

  void commit(std::size_t n) noexcept {
    write_pos_ = std::min(write_pos_ + n, N);
  }

  void clear() noexcept {
    read_pos_ = 0;
    write_pos_ = 0;
  }

  void compact() {
    if (read_pos_ == 0) return;

    auto const sz = size();
    if (sz > 0) {
      std::memmove(storage_.data(), storage_.data() + read_pos_, sz);
    }
    read_pos_ = 0;
    write_pos_ = sz;
  }

 private:
  std::array<char, N> storage_;
  std::size_t read_pos_ = 0;
  std::size_t write_pos_ = 0;
};

}  // namespace xz::io

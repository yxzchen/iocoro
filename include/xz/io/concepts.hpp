#pragma once

#include <concepts>
#include <coroutine>
#include <system_error>

namespace xz::io {

/// Concept for completion handlers that take an error_code
template <typename T>
concept completion_handler = requires(T t, std::error_code ec) {
  { t(ec) } -> std::same_as<void>;
};

/// Concept for read completion handlers
template <typename T>
concept read_handler = requires(T t, std::error_code ec, std::size_t bytes) {
  { t(ec, bytes) } -> std::same_as<void>;
};

/// Concept for write completion handlers
template <typename T>
concept write_handler = requires(T t, std::error_code ec, std::size_t bytes) {
  { t(ec, bytes) } -> std::same_as<void>;
};

/// Concept for awaitable types
template <typename T>
concept awaitable = requires(T t) {
  { t.await_ready() } -> std::convertible_to<bool>;
  { t.await_suspend(std::coroutine_handle<>{}) };
  { t.await_resume() };
};

}  // namespace xz::io

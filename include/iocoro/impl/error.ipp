#include <iocoro/error.hpp>

namespace iocoro {

namespace detail {

class error_category_impl : public std::error_category {
 public:
  auto name() const noexcept -> char const* override { return "iocoro"; }

  auto message(int ev) const -> std::string override {
    switch (static_cast<error>(ev)) {
      case error::operation_aborted:
        return "operation aborted";
      default:
        return "unknown error";
    }
  }
};

inline auto error_category() -> std::error_category const& {
  static error_category_impl instance;
  return instance;
}

}  // namespace detail

auto make_error_code(error e) -> std::error_code {
  return {static_cast<int>(e), detail::error_category()};
}

}  // namespace iocoro
